#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -DNVG_NO_STB -Isrc src/nanovg.c regress/baseline.c -lm -o "$b/baseline"
"$b/baseline"
