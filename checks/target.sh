#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/tinygl.XXXXXX")
trap 'rm -rf "$b"' EXIT
for f in src/*.c checks/scene.c; do
 cc -std=c99 -O1 -Wno-constant-conversion -Iinclude -Isrc -c "$f" -o "$b/$(basename "$f" .c).o"
done
ar rcs "$b/libtinygl.a" "$b"/*.o
c++ -std=c++17 -O2 -Iinclude checks/target_main.cpp checks/target_tests.cpp "$b/libtinygl.a" -lm -o "$b/run"
"$b/run"
bash checks/baseline.sh
