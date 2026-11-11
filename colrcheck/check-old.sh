#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/colr-old.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I. -DTT_TEST -DSTB_RECT_PACK_IMPLEMENTATION -DSTB_IMAGE_WRITE_IMPLEMENTATION tests/test_truetype.c -lm -o "$build/upstream-truetype"
"$build/upstream-truetype" 64 32 colrcheck/geometry.ttf > "$build/truetype.out"
test -s "$build/truetype.out"
grep -q '@' "$build/truetype.out"
printf 'UPSTREAM_TRUETYPE_OK ascii_rows=%s\n' "$(wc -l < "$build/truetype.out" | tr -d ' ')"
cc -std=c99 -I. -c tests/test_truetype.c -o "$build/truetype.o"
c++ -std=c++11 -x c++ -I. -c tests/test_truetype.c -o "$build/truetype-cpp.o"
printf 'UPSTREAM_DECLARATIONS_OK languages=C99,C++11\n'
