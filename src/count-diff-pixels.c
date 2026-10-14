/*
 *    Number of differing pixels between two PNG images.
 *
 *    A drop-in C replacement for the Python "count-diff-pixels" helper: it
 *    prints the count of pixels with at least one channel differing by more
 *    than --threshold, and exits 0 on success. Only libpng and libm are
 *    needed:
 *
 *      cc -O2 -o count-diff-pixels count-diff-pixels.c \
 *          $(pkg-config --cflags --libs libpng) -lm
 *
 *    Both images are reduced to packed 8-bit RGB, the same representation
 *    PIL's convert("RGB") hands the Python helper. A channel difference is
 *    computed in int, not on unsigned bytes, so a -1 difference is 1 and not
 *    255.
 */

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
  fprintf(stderr, "count-diff-pixels: %s\n", what);
  exit(1);
}

typedef struct
{
  unsigned char *data; /* packed 8-bit RGB */
  unsigned int width;
  unsigned int height;
  size_t rowbytes;
} image_t;

static image_t read_png(const char *path)
{
  image_t img = { 0 };

  FILE *fp = fopen(path, "rb");
  if(!fp) fail(path);

  png_byte header[8];
  if(fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
  {
    fclose(fp);
    fail(path);
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png)
  {
    fclose(fp);
    fail(path);
  }
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    fail(path);
  }
  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    fail(path);
  }

  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);

  png_uint_32 width, height;
  int bit_depth, color_type;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);

  /* bring every input to packed 8-bit RGB */
  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if(png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if(color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_strip_alpha(png);
  if(bit_depth == 16)
    png_set_strip_16(png);

  png_read_update_info(png, info);

  img.width = width;
  img.height = height;
  img.rowbytes = png_get_rowbytes(png, info);

  if(png_get_channels(png, info) != 3 || png_get_bit_depth(png, info) != 8)
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    fail(path);
  }

  img.data = malloc((size_t)img.rowbytes * img.height);
  png_bytep *rows = malloc(sizeof(png_bytep) * img.height);
  if(!img.data || !rows)
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    fail(path);
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

int main(int argc, char **argv)
{
  const char *paths[2] = { NULL, NULL };
  int npaths = 0;
  long threshold = 0;

  for(int i = 1; i < argc; i++)
  {
    if(strcmp(argv[i], "--threshold") == 0 && i + 1 < argc)
      threshold = strtol(argv[++i], NULL, 10);
    else if(strncmp(argv[i], "--threshold=", 12) == 0)
      threshold = strtol(argv[i] + 12, NULL, 10);
    else if(npaths < 2)
      paths[npaths++] = argv[i];
  }

  if(npaths != 2)
  {
    fprintf(stderr, "usage: %s <image1> <image2> [--threshold N]\n", argv[0]);
    return 1;
  }

  const image_t a = read_png(paths[0]);
  const image_t b = read_png(paths[1]);

  if(a.width != b.width || a.height != b.height)
    fail("images must have the same dimensions");

  long long count = 0;

  for(unsigned int y = 0; y < a.height; y++)
  {
    const unsigned char *ra = a.data + (size_t)y * a.rowbytes;
    const unsigned char *rb = b.data + (size_t)y * b.rowbytes;

    for(unsigned int x = 0; x < a.width; x++)
    {
      const unsigned char *pa = ra + (size_t)x * 3;
      const unsigned char *pb = rb + (size_t)x * 3;

      for(int c = 0; c < 3; c++)
      {
        int d = (int)pa[c] - (int)pb[c];
        if(d < 0) d = -d;
        if(d > threshold)
        {
          count++;
          break;
        }
      }
    }
  }

  printf("%lld\n", count);
  return 0;
}
