#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -Isrc -Iharness src/*.c harness/main.c tests/text_anchor_contract.c -lm -o "$build/target"
"$build/target"
bash harness/baseline.sh
