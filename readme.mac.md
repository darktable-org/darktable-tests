# Instructions for macOS 
MacOS is shipped with its own version of python, getopt and is not using bash but zsh. 
This gives issues with the python environment and the bash script when running the tests. 

This readme assumes that you have installed brew and are able to build and run darktable.

## Install brew dependencies.
The standard macOS tools are given issues, so install GNU versions.

```bash
brew install coreutils
brew install gnu-getopt
brew install bash
```

## Create the python environment 
The next step is to create a python environment and install the required packages.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip3 install imageio colour-science packaging numpy Pillow scipy matplotlib
```

## Run the tests
Once this is ensured, the right paths must be set before the test can run. Additionally, 
reactive the python virtual evn if not already done.

```bash
export DARKTABLE_CLI="$PWD/../../../build/bin/darktable-cli"
export PATH="/opt/homebrew/opt/gnu-getopt/bin:/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH"
export XDG_DATA_DIRS="/opt/homebrew/share:/usr/local/share:/usr/share"
source ./.venv/bin/activate
/opt/homebrew/bin/bash ./run 0001-exposure
```