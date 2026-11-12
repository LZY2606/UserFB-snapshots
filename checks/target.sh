#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -D__LITTLE_ENDIAN__ -std=c++17 -O2 -I. -Ichecks -Iinclude tests/target_regression.cpp checks/target_main.cpp src/Stk.cpp src/FileRead.cpp src/FileWvIn.cpp src/FileLoop.cpp -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP metadata assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP sustain assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP storage assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1
bash checks/baseline.sh
