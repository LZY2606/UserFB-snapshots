#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c mini3d.c -o "$b/core.o"
if test -f csm.c; then cc -std=c99 -O2 -I. -c csm.c -o "$b/csm.o"; else cc -x c -c /dev/null -o "$b/csm.o"; fi
c++ -std=c++17 -O2 -I. -Isupport support/target_main.cpp tests/csm_cases.cpp "$b/core.o" "$b/csm.o" -lm -o "$b/target"
"$b/target"
