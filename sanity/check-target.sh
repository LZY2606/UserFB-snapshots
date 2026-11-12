#!/usr/bin/env bash
set -euo pipefail
test -s sanity/target.cpp || { echo 'missing sanity/target.cpp' >&2; exit 1; }
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-lexbor-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
bash sanity/build.sh "$b"
c++ -std=c++17 -I source -I sanity sanity/target.cpp sanity/target_main.cpp "$b/liblexbor_static.a" -lm -o "$b/target"
"$b/target"
cc -std=c99 -I source sanity/baseline.c "$b/liblexbor_static.a" -lm -o "$b/witness"
"$b/witness"
