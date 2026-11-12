#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -DNO_OMP -Isrc checks/baseline.cpp -o "$b/base"
"$b/base" "$b/flat.pfm"
test -s "$b/flat.pfm"
echo VCM_IMAGE_WRITTEN_OK
