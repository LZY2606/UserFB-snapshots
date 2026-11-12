#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -D__LITTLE_ENDIAN__ -Iinclude checks/baseline.cpp src/Stk.cpp src/FileRead.cpp src/FileWvIn.cpp src/FileLoop.cpp -o "$build/baseline"
"$build/baseline" "$build"
