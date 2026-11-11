#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b/build"
"${CXX:-c++}" -std=c++11 -I. support/baseline.cpp "$b/build/gme/libgme.a" -o "$b/baseline"
if [ "${1:-}" = normal ]; then "$b/baseline" --normal; else "$b/baseline"; fi
for t in basics basics_mem; do
  "${CXX:-c++}" -std=c++11 -I. -x c++ "test/$t.c" test/Wave_Writer.cpp -x none "$b/build/gme/libgme.a" -o "$b/$t"
  "$b/$t" test.nsf "$b/$t.wav"
  test -s "$b/$t.wav"
done
cmp "$b/basics.wav" "$b/basics_mem.wav"
printf 'VGM_OLD_NSF_FILE_MEMORY_OK\n'
