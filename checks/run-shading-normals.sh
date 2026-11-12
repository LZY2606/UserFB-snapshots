#!/bin/sh
set -eu
test -s checks/shading_normals_test.cpp
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -DNO_OMP -Isrc checks/shading_normals_test.cpp -o "$b/test"
"$b/test" "$b" > "$b/out"
cat "$b/out"
for mark in VCM_NORMAL_INTERPOLATION_OK VCM_ADJOINT_OK VCM_GEOMETRIC_SUPPORT_OK VCM_PDF_CONSISTENCY_OK VCM_SMOOTH_RENDER_OK; do
 grep -Fx "$mark" "$b/out" >/dev/null
done
test -s "$b/smooth.pfm"
echo VCM_SHADING_NORMALS_OK
