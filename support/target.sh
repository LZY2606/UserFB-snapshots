#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
if test -f ambient_occlusion.cpp; then c++ -std=c++20 -O2 -I. -ICpp/Source -Isrc/renderer -c ambient_occlusion.cpp -o "$b/newmodule.o"; else c++ -x c++ -c /dev/null -o "$b/newmodule.o"; fi
c++ -std=c++20 -O2 -I. -Isupport support/target_main.cpp tests/ao_cases.cpp our_gl.cpp model.cpp tgaimage.cpp "$b/newmodule.o" -o "$b/target"
"$b/target"
