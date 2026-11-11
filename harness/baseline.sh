#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d "${TMPDIR:-/tmp}/fontstash-baseline.XXXXXX")
trap 'rm -rf "$d"' EXIT HUP INT TERM
cc -std=c99 -O2 -Isrc harness/baseline.c -lm -o "$d/baseline"
"$d/baseline"
cc -std=c99 -O2 -Isrc -Iharness harness/legacy_gl.c -lm -o "$d/legacy-gl"
"$d/legacy-gl"
