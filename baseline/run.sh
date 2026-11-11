#!/bin/bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O0 -Iinclude src/*.c baseline/witness.c -o "$build/witness"
"$build/witness"
