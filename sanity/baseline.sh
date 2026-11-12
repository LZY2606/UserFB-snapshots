#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-lexbor.XXXXXX")
trap 'rm -rf "$b"' EXIT
bash sanity/build.sh "$b"
cc -std=c99 -I source sanity/baseline.c "$b/liblexbor_static.a" -lm -o "$b/witness"
"$b/witness"
