#!/usr/bin/env bash
set -euo pipefail
mkdir -p build
cc --version | head -n 1
c++ --version | head -n 1
cc -std=c99 -I. -c tests/test_truetype.c -o build/upstream-truetype.o
c++ -x c++ -std=c++11 -I. -c tests/test_truetype.c -o build/upstream-truetype-cpp.o
printf 'TOOLCHAIN_OK no_downloads external_packages=0\n'
