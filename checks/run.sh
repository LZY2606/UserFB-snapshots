#!/bin/bash
set -euo pipefail
file=${1:-checks/baseline.cpp}
test -s "$file"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O2 -I. "$file" FFTConvolver.cpp TwoStageFFTConvolver.cpp AudioFFT.cpp Utilities.cpp -o "$build/check"
"$build/check"
