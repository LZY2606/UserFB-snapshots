#!/bin/bash
set -euo pipefail
file=${1:-sanity/baseline.c}
test -s "$file"
build=$(mktemp -d "${TMPDIR:-/tmp}/ebur128-sanity.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -D_GNU_SOURCE -O2 -I ebur128 ebur128/ebur128.c "$file" -lm -o "$build/check"
"$build/check"
