#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-xsd.XXXXXX")
trap 'rm -rf "$b"' EXIT
bash regress/build.sh "$b"
cc -std=c99 -I include -I "$b" regress/baseline.c "$b/libxml2.a" -lm -pthread -o "$b/witness"
"$b/witness"
