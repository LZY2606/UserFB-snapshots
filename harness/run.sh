#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1
test -s "$1"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -Iinclude -Isrc "$1" src/*.c -lm -o "$build/check"
"$build/check"
