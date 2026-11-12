#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/target.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
build_case tests/vbr_full_range.c devcheck/target_main.c
"$b/case" > "$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
test "$(grep -Ec '^TARGET_CASE selection assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE syntax assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE table_switch assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE compatibility assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Fxc 'TARGET_OK groups=4' "$b/result")" = 1
bash devcheck/baseline.sh
