#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1
test -s "$1"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c11 -O1 -DPLUTOVG_BUILD_STATIC -Iinclude -Isource "$1" source/*.c -lm -o "$build/check"
"$build/check"
