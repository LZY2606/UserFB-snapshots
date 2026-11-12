#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -I. -Ichecks -ISource -ISource/Utility tests/target_regression.cpp checks/target_main.cpp Source/Effects/overdrive.cpp Source/Effects/wavefolder.cpp -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP integral assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP modulation assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP compatibility assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1
bash checks/baseline.sh
