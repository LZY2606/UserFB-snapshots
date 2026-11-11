#!/usr/bin/env bash
set -euo pipefail
test -s tests/opacity_contract.c
build=$(mktemp -d)
trap 'cmake -E remove_directory "$build"' EXIT
cc -std=c99 -O2 -Wall -Wextra tests/opacity_contract.c -lm -o "$build/opacity"
"$build/opacity" | tee "$build/result.log"
for marker in OPACITY_GROUP_OK OPACITY_NESTING_OK OPACITY_ELEMENT_OK OPACITY_REUSE_OK; do grep -Fxq "$marker" "$build/result.log"; done
cc -std=c99 -O2 -Wall -Wextra support/legacy.c -lm -o "$build/legacy"
"$build/legacy"
echo NSVG_OPACITY_TARGET_OK
