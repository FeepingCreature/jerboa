#!/bin/sh
set -e
cd build/
cmake ..
make
ctest -V
