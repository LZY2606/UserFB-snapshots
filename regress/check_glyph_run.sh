#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -DNVG_NO_STB -Isrc src/nanovg.c regress/check_glyph_run.c -lm -o "$b/check_glyph_run"
"$b/check_glyph_run"
