#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/sndfilter-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=gnu99 -Isrc checks/baseline.c src/compressor.c -lm -o "$b/check"
"$b/check"
