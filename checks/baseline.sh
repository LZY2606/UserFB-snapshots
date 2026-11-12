#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c11 -O2 -Iinclude -Isrc checks/baseline.c src/*.c -lm -o "$b/baseline"
"$b/baseline"
