#!/bin/sh
set -eu
test -s checks/deformation_reference.cpp
test -s checks/check-deformation-mutants.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -I. -c ufbx.c -o "$b/ufbx.o"
c++ -std=c++11 -O2 -I. checks/deformation_reference.cpp "$b/ufbx.o" -lm -o "$b/test"
"$b/test" "$b" > "$b/out"
cat "$b/out"
for mark in UFBX_DQ_REFERENCE_OK UFBX_BLEND_SKIN_ORDER_OK UFBX_DEFORMED_NORMALS_OK UFBX_SCENE_REFERENCE_OK; do
 grep -Fx "$mark" "$b/out" >/dev/null
done
test -s "$b/deformation.obj"
sh checks/check-deformation-mutants.sh
