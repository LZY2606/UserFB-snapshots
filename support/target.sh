#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O2 -Dmain=tinyraytracer_legacy_main -c tinyraytracer.cpp -o "$b/core.o"
if test -f volume.cpp; then c++ -std=c++17 -O2 -I. -c volume.cpp -o "$b/volume.o"; else c++ -x c++ -c /dev/null -o "$b/volume.o"; fi
c++ -std=c++17 -O2 -I. -Isupport support/target_main.cpp tests/fog_cases.cpp "$b/core.o" "$b/volume.o" -o "$b/target"
"$b/target"
