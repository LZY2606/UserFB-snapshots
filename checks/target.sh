#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c17 -O2 -I. -Ichecks tests/target_regression.c checks/target_main.c  -lm -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP layers assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP admission assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP channel_audio assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1
bash checks/baseline.sh
