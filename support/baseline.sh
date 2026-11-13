#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
c++ -std=c++17 -O2 -D__EMSCRIPTEN__ -include cmath -include cstring -I. support/baseline.cpp Cpp/Source/Maths.cpp -o .build/baseline
.build/baseline
