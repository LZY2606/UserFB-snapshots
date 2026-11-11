#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
"${CXX:-c++}" -std=c++11 -O2 -I. test/StreamSmoke.cpp FFTConvolver.cpp AudioFFT.cpp Utilities.cpp -o build/stream_smoke
./build/stream_smoke
