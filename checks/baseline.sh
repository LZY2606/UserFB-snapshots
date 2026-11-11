#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=gnu99 -Isrc checks/baseline.c src/compressor.c -lm -o "$b/check"
"$b/check"
