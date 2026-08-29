# darktable integration tests

This repository hosts the [darktable](https://www.darktable.org/)
integration tests. Each test renders a raw image from `images/` through a
fixed XMP sidecar with `darktable-cli` and compares the result against a
committed reference image, both on the CPU and the OpenCL path.

## Test setup

```
run                : main driver, runs the tests
requirements.txt   : Python dependencies of the helper scripts

deltae             : CIE 2000 delta-E report between two images (Python)
count-diff-pixels  : number of differing pixels between two images (Python)
check-performance  : reports timing regressions from logs/perfs.log
check-failures     : interactive review of the failures of a log
check-cpu-gpu      : refreshes the cpugpu.maxpix thresholds from a log

images/            : test images
logs/              : run logs, timings (perfs.log) and their analysis
nnnn-name/         : one test
```

A test directory contains:

| file               |                                                        |
|--------------------|--------------------------------------------------------|
| `<name>.xmp`       | the sidecar to apply, names the image via `DerivedFrom` |
| `expected.png`     | the reference output, created on the first run          |
| `cpugpu.maxpix`    | max tolerated CPU/GPU pixel difference                  |
| `CONFIG`           | optional, one extra `darktable-cli` conf option per line|
| `README`           | optional, first line is a label shown next to the test  |
| `test.sh`          | optional, replaces the default driver, returns 0 if OK  |

A test passes when the max delta-E against `expected.png` stays below 2.3
(and the average below 2.3/3).

### Requirements

* `darktable-cli`, found either in the `PATH`, through the `DARKTABLE_CLI`
  environment variable, or in `/opt/darktable`
* `compare` (ImageMagick), for the diff images
* `zopflipng`, only needed to create the `expected.png` of a new test
* `loupe`, only needed by `check-failures`
* Python 3 with the modules listed in `requirements.txt`

### Python virtual environment

`deltae` and `count-diff-pixels` need a few Python modules. `./run` creates
the virtual environment `.venv` on its first run, installs
`requirements.txt` into it and activates it, so there is nothing to do.

To create it by hand, or to use the scripts outside of `./run`:

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

Two environment variables control this:

```
DARKTABLE_TESTS_VENV   use another location than ./.venv
NO_VENV=1              do not use a virtual environment, the modules are
                       then expected to be already available
```

## Running the tests

```bash
./run                     # all tests
./run 0001-exposure       # a single test
./run --op=exposure       # all tests using that module
```

Options:

```
--disable-opencl           do not run the OpenCL path
--disable-timing           do not output timing
--no-deltae                light check, does not need the delta-E module
--fast-fail                stop on the first failing test
--op=<n> | --operation=<n> run the tests with matching operation n
--gdb | --gdb-cl           run the CPU / OpenCL path under gdb
```

Every run writes `logs/test-<date>.log`, appends the timings to
`logs/perfs.log` and finally reports timing regressions. To review the
failures of a run:

```bash
./check-failures logs/test-<date>.log
```

## Adding a test with the default driver

1. Create the directory `<nnnn>-<meaningful name>`.
2. Develop one of the test images in darktable, then copy its XMP into that
   directory as `<meaningful name>.xmp`.
3. Optionally add a `README` and a `CONFIG` (see the table above).
4. Run `./run <dir>` a first time: as there is no `expected.png` yet, the
   output is optimized with `zopflipng` and saved as the reference. Check
   that it really is the expected output.
5. Run `./run <dir>` again, everything must now be 0:

```
Test 0001-exposure
      Image mire1.cr2
      CPU & GPU version differ by 25699 pixels
      ----------------------------------
      Max dE                   : 0.00000
      Avg dE                   : 0.00000
      Std dE                   : 0.00000
      ----------------------------------
      ...
      Pixels above tolerance   : 0.00 %
  OK
```

6. Commit the `.xmp` and the `expected.png`.

## Adding a test with a specific driver

Create `<nnnn>-<meaningful name>/test.sh` instead. It may do whatever the
test needs and must return 0 when the test passes, 1 otherwise.
