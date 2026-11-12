#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d);trap 'rm -rf "$b"' EXIT
bash regress/build.sh "$b"
cc -std=c99 -I src regress/baseline.c "$b/src/libsoxr.a" -lm -o "$b/baseline"
"$b/baseline"
ctest --test-dir "$b" -R '^1-delay-clear$' --output-on-failure
