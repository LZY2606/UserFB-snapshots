#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/libcaption-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cp src/eia608_from_utf8.c.cached "$b/eia608_from_utf8.c"
cc -std=c99 -O1 -I. -Icaption verification/baseline.c src/caption.c src/eia608.c src/eia608_charmap.c src/utf8.c src/xds.c "$b/eia608_from_utf8.c" -o "$b/witness"
"$b/witness"
