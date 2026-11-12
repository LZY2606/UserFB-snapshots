#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p gsubcheck/.build
${CC:-cc} -std=c99 -O2 -I. gsubcheck/baseline.c -lm -o gsubcheck/.build/baseline
gsubcheck/.build/baseline gsubcheck/.build/geometry.ttf
${CC:-cc} -std=c99 -O2 -I. -c gsubcheck/old_driver.c -o gsubcheck/.build/old-driver.o
${CXX:-c++} -std=c++11 -O2 -I. gsubcheck/old_ds.cpp gsubcheck/.build/old-driver.o -lm -o gsubcheck/.build/old-truetype
gsubcheck/.build/old-truetype >gsubcheck/.build/old-truetype.log
echo OLD_TRUETYPE_MAIN_OK
