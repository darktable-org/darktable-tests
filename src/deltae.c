/*
 *    CIE 2000 delta-E report between two PNG images.
 *
 *    A drop-in C replacement for the Python "deltae" helper: same report on
 *    stdout and the same exit codes (2 = above tolerance, 1 = changed but
 *    within tolerance, 0 = bit identical). Only libpng and libm are needed:
 *
 *      cc -O2 -o deltae deltae.c $(pkg-config --cflags --libs libpng) -lm
 *
 *    The colour pipeline mirrors colour-science, which the Python helper uses:
 *    sRGB (nonlinear) -> linear -> CIE XYZ (D65) -> CIE Lab (D65), followed by
 *    the CIE 2000 colour difference with parametric factors kL=kC=kH=1.
 */

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_DELTA_E 2.3
#define MAX_AVG_DELTA_E (MAX_DELTA_E / 3.0)

#define DEG2RAD 0.017453292519943295
#define POW25_7 6103515625.0 /* 25^7, appears in the CIEDE2000 weights */

/* CIE XYZ of the D65 white point, from x=0.3127 y=0.3290 and Y=1 */
#define XN 0.9504559270516717
#define YN 1.0
#define ZN 1.0890577507598784

static void die(const char *what)
{
  fprintf(stderr, "deltae: %s\n", what);
  exit(2);
}

typedef struct
{
  unsigned char *data;
  unsigned int width;
  unsigned int height;
  size_t rowbytes;
  int bit_depth;
} image_t;

static image_t read_png(const char *path)
{
  image_t img = { 0 };

  FILE *fp = fopen(path, "rb");
  if(!fp) die(path);

  png_byte header[8];
  if(fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
  {
    fclose(fp);
    die(path);
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png)
  {
    fclose(fp);
    die(path);
  }
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    die(path);
  }
  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    die(path);
  }

  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);

  png_uint_32 width, height;
  int bit_depth, color_type;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);

  /* bring every input to packed RGB at its source bit depth */
  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if(png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if(color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_strip_alpha(png);

  png_read_update_info(png, info);

  img.width = width;
  img.height = height;
  img.bit_depth = png_get_bit_depth(png, info);
  img.rowbytes = png_get_rowbytes(png, info);

  if(png_get_channels(png, info) != 3 ||
     (img.bit_depth != 8 && img.bit_depth != 16))
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    die(path);
  }

  img.data = malloc((size_t)img.rowbytes * img.height);
  png_bytep *rows = malloc(sizeof(png_bytep) * img.height);
  if(!img.data || !rows)
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    die(path);
  }
  for(unsigned int y = 0; y < img.height; y++)
    rows[y] = img.data + (size_t)y * img.rowbytes;

  png_read_image(png, rows);
  png_read_end(png, NULL);
  free(rows);

  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  return img;
}

/* sRGB decoding curve, tabulated for every possible source sample */
static double *build_decode_lut(const int bit_depth)
{
  const int n = 1 << bit_depth;
  const double max = (double)(n - 1);
  double *lut = malloc(sizeof(double) * n);
  if(!lut) die("out of memory");

  for(int i = 0; i < n; i++)
  {
    const double c = i / max;
    lut[i] = (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
  }
  return lut;
}

static inline double lab_f(const double t)
{
  static const double d = 6.0 / 29.0;
  return (t > d * d * d) ? cbrt(t) : t / (3.0 * d * d) + 4.0 / 29.0;
}

static inline void linear_to_lab(const double lin[3], double lab[3])
{
  const double X = 0.4124 * lin[0] + 0.3576 * lin[1] + 0.1805 * lin[2];
  const double Y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
  const double Z = 0.0193 * lin[0] + 0.1192 * lin[1] + 0.9505 * lin[2];
  const double fx = lab_f(X / XN);
  const double fy = lab_f(Y / YN);
  const double fz = lab_f(Z / ZN);
  lab[0] = 116.0 * fy - 16.0;
  lab[1] = 500.0 * (fx - fy);
  lab[2] = 200.0 * (fy - fz);
}

static inline double pow7(const double c)
{
  const double c3 = c * c * c;
  return c3 * c3 * c;
}

/* CIE 2000 colour difference, as in colour.difference.delta_E_CIE2000 */
static inline double ciede2000(const double L1, const double a1, const double b1,
                               const double L2, const double a2, const double b2)
{
  const double C1 = hypot(a1, b1);
  const double C2 = hypot(a2, b2);
  const double Cbar7 = pow7((C1 + C2) * 0.5);
  const double G = 0.5 * (1.0 - sqrt(Cbar7 / (Cbar7 + POW25_7)));
  const double ap1 = (1.0 + G) * a1;
  const double ap2 = (1.0 + G) * a2;
  const double Cp1 = hypot(ap1, b1);
  const double Cp2 = hypot(ap2, b2);

  double hp1 = (b1 == 0.0 && ap1 == 0.0) ? 0.0 : fmod(atan2(b1, ap1) / DEG2RAD, 360.0);
  if(hp1 < 0.0) hp1 += 360.0;
  double hp2 = (b2 == 0.0 && ap2 == 0.0) ? 0.0 : fmod(atan2(b2, ap2) / DEG2RAD, 360.0);
  if(hp2 < 0.0) hp2 += 360.0;

  const double dLp = L2 - L1;
  const double dCp = Cp2 - Cp1;
  const double Cp1m2 = Cp1 * Cp2;

  double dhp = 0.0;
  if(Cp1m2 != 0.0)
  {
    const double d = hp2 - hp1;
    if(fabs(d) <= 180.0)
      dhp = d;
    else
      dhp = (d > 180.0) ? d - 360.0 : d + 360.0;
  }
  const double dHp = 2.0 * sqrt(Cp1m2) * sin(dhp * 0.5 * DEG2RAD);

  const double Lbar = (L1 + L2) * 0.5;
  const double Cbarp = (Cp1 + Cp2) * 0.5;
  const double sum_h = hp1 + hp2;
  const double diff_h = fabs(hp1 - hp2);

  double hbar;
  if(Cp1m2 == 0.0)
    hbar = sum_h;
  else if(diff_h <= 180.0)
    hbar = sum_h * 0.5;
  else if(sum_h < 360.0)
    hbar = (sum_h + 360.0) * 0.5;
  else
    hbar = (sum_h - 360.0) * 0.5;

  const double T = 1.0 - 0.17 * cos((hbar - 30.0) * DEG2RAD)
                       + 0.24 * cos(2.0 * hbar * DEG2RAD)
                       + 0.32 * cos((3.0 * hbar + 6.0) * DEG2RAD)
                       - 0.20 * cos((4.0 * hbar - 63.0) * DEG2RAD);
  const double dtheta = 30.0 * exp(-(hbar - 275.0) * (hbar - 275.0) / 625.0);
  const double RC = 2.0 * sqrt(pow7(Cbarp) / (pow7(Cbarp) + POW25_7));
  const double Lbar2 = (Lbar - 50.0) * (Lbar - 50.0);
  const double SL = 1.0 + (0.015 * Lbar2) / sqrt(20.0 + Lbar2);
  const double SC = 1.0 + 0.045 * Cbarp;
  const double SH = 1.0 + 0.015 * Cbarp * T;
  const double RT = -sin(2.0 * dtheta * DEG2RAD) * RC;

  const double dL = dLp / SL;
  const double dC = dCp / SC;
  const double dH = dHp / SH;
  return sqrt(dL * dL + dC * dC + dH * dH + RT * dC * dH);
}

int main(int argc, char **argv)
{
  if(argc != 3)
  {
    fprintf(stderr, "usage: %s <expected.png> <output.png>\n", argv[0]);
    return 2;
  }

  const image_t expected = read_png(argv[1]);
  const image_t output = read_png(argv[2]);

  if(expected.width != output.width || expected.height != output.height)
  {
    fprintf(stderr, "deltae: image sizes differ, %ux%u vs %ux%u\n", expected.width,
            expected.height, output.width, output.height);
    return 2;
  }

  const size_t npix = (size_t)expected.width * expected.height;
  const double *lut_e = build_decode_lut(expected.bit_depth);
  const double *lut_o = build_decode_lut(output.bit_depth);

  double *de = malloc(sizeof(double) * npix);
  if(!de) die("out of memory");

  const int ppe = expected.bit_depth / 8; /* bytes per sample */
  const int ppo = output.bit_depth / 8;

  double sum = 0.0, sumsq = 0.0, maxde = 0.0;

  for(unsigned int y = 0; y < expected.height; y++)
  {
    const unsigned char *re = expected.data + (size_t)y * expected.rowbytes;
    const unsigned char *ro = output.data + (size_t)y * output.rowbytes;

    for(unsigned int x = 0; x < expected.width; x++)
    {
      const unsigned char *pe = re + (size_t)x * 3 * ppe;
      const unsigned char *po = ro + (size_t)x * 3 * ppo;

      double lin_e[3], lin_o[3];
      if(ppe == 2)
        for(int c = 0; c < 3; c++) lin_e[c] = lut_e[(pe[2 * c] << 8) | pe[2 * c + 1]];
      else
        for(int c = 0; c < 3; c++) lin_e[c] = lut_e[pe[c]];
      if(ppo == 2)
        for(int c = 0; c < 3; c++) lin_o[c] = lut_o[(po[2 * c] << 8) | po[2 * c + 1]];
      else
        for(int c = 0; c < 3; c++) lin_o[c] = lut_o[po[c]];

      double lab_e[3], lab_o[3];
      linear_to_lab(lin_e, lab_e);
      linear_to_lab(lin_o, lab_o);

      const double d = ciede2000(lab_e[0], lab_e[1], lab_e[2], lab_o[0], lab_o[1], lab_o[2]);
      de[(size_t)y * expected.width + x] = d;
      sum += d;
      sumsq += d * d;
      if(d > maxde) maxde = d;
    }
  }

  const double num = (double)npix;
  const double mean = sum / num;
  double var = sumsq / num - mean * mean;
  if(var < 0.0) var = 0.0;
  const double std = sqrt(var);

  long long c0 = 0, c1 = 0, c3 = 0, c6 = 0, c9 = 0, above = 0;
  for(size_t i = 0; i < npix; i++)
  {
    const double d = de[i];
    if(d <= mean) c0++;
    if(d <= mean + std) c1++;
    if(d <= mean + 3.0 * std) c3++;
    if(d <= mean + 6.0 * std) c6++;
    if(d <= mean + 9.0 * std) c9++;
    if(d >= MAX_DELTA_E) above++;
  }

  printf("      ----------------------------------\n");
  printf("      Max dE                   : %.5f\n", maxde);
  printf("      Avg dE                   : %.5f\n", mean);
  printf("      Std dE                   : %.5f\n", std);
  printf("      ----------------------------------\n");
  printf("      Pixels below avg + 0 std : %.2f %%\n", (double)c0 / num * 100.0);
  printf("      Pixels below avg + 1 std : %.2f %%\n", (double)c1 / num * 100.0);
  printf("      Pixels below avg + 3 std : %.2f %%\n", (double)c3 / num * 100.0);
  printf("      Pixels below avg + 6 std : %.2f %%\n", (double)c6 / num * 100.0);
  printf("      Pixels below avg + 9 std : %.2f %%\n", (double)c9 / num * 100.0);
  printf("      ----------------------------------\n");
  printf("      Pixels above tolerance   : %.2f %%\n", (double)above / num * 100.0);

  if(maxde > MAX_DELTA_E || mean > MAX_AVG_DELTA_E)
    return 2;
  if(maxde < 0.01)
    return 0;
  return 1;
}
