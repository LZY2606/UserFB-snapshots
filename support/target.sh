#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh tests/test_aes_bitslice.c support/driver.c -o "$build/target"
"$build/target"
bash support/baseline.sh
