#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -Isrc checks/baseline.cpp src/maximilian.cpp src/libs/PolyBLEP/PolyBLEP.cpp -o "$b/check"
"$b/check"
