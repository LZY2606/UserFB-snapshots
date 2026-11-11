#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -D_STK_DEBUG_ -Iinclude -Isanity sanity/baseline.cpp src/Stk.cpp src/SineWave.cpp src/Voicer.cpp -o "$build/baseline"
"$build/baseline"
