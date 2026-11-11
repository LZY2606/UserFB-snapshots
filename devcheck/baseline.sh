#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
mkdir -p build
cc -std=c99 -O1 -g -I. devcheck/current.c -lm -o build/compound-current
build/compound-current
set +e
build/compound-current --anchor >build/anchor.stdout 2>build/anchor.stderr
rc=$?
set -e
if [[ $rc != 134 ]] || ! grep -F 'stbtt__GetGlyphShapeTT' build/anchor.stderr >/dev/null; then
  printf 'Unexpected anchor witness exit=%s\n' "$rc"; exit 1
fi
printf 'ANCHOR_ASSERT_CONFIRMED exit=%s\n' "$rc"
build/compound-current --font
cc -std=c99 -O1 -g -I. devcheck/allocation-current.c -lm -o build/allocation-current
build/allocation-current
printf 'COMPOUND_BASELINE_OK\n'
