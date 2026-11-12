#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -ffp-contract=off -I. -Ichecks -Isrc tests/target_regression.cpp checks/target_main.cpp  -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP numeric assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP voices assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP native assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1
c++ -std=c++17 -O2 -ffp-contract=off -DMOOG_BANK_FORCE_SCALAR=1 -I. -Ichecks -Isrc tests/target_regression.cpp checks/target_main.cpp -o "$build/scalar"
"$build/scalar" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP numeric assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP voices assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP native assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1
bash checks/baseline.sh
