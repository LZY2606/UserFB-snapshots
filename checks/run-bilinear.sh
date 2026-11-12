#!/bin/sh
set -eu
test -s checks/bilinear_test.cpp
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -I. checks/bilinear_test.cpp -o "$b/test"
"$b/test" "$b" > "$b/out"
cat "$b/out"
for mark in BILINEAR_PLANAR_OK BILINEAR_TWO_ROOTS_OK BILINEAR_TRIM_OK BILINEAR_BVH_OK BILINEAR_RENDER_OK; do
  grep -Fx "$mark" "$b/out" >/dev/null
done
test -s "$b/bilinear.ppm"
echo BILINEAR_SUITE_OK
