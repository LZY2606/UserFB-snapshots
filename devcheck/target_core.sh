#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/target.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
TTA_INTERNAL=1 build_case tests/entropy_reference.c devcheck/target_main.c
"$b/case" > "$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
test "$(grep -Ec '^TARGET_CASE thresholds assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE fifo assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE frames assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Fxc 'TARGET_OK groups=3' "$b/result")" = 1
bash devcheck/baseline.sh
