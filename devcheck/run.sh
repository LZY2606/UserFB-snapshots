#!/bin/bash
set -euo pipefail
file=${1:-devcheck/baseline.c}
test -s "$file"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I. "$file" -lm -o "$build/native"
"$build/native"
cc -std=c99 -O2 -DMINIMP3_NO_SIMD -I. "$file" -lm -o "$build/scalar"
"$build/scalar"
printf '%s\n' MINIMP3_NATIVE_AND_SCALAR_OK
