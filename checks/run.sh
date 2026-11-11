#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1
test -s "$1"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -DNVG_NO_STB -I src "$1" src/nanovg.c -lm -o "$build/check"
"$build/check"
