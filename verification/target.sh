#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s tests/lz77_index.cpp
c++ -std=c++11 -O2 verification/reference_runner.cpp -o "$b/stock"
c++ -std=c++11 -O2 -I.  tests/lz77_index.cpp verification/driver_main.cpp -o "$b/target"
STOCK_ENCODER="$b/stock" "$b/target" > "$b/result"
cat "$b/result"
test "$(grep -Fxc 'CASE_PASS legacy-stream' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS lazy-window' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS block-history' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS index-work' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS png-path' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS c-compatibility' "$b/result")" = 1
grep -Eq '^EXECUTED ([1-9][0-9]*)$' "$b/result"
echo TARGET_ALL_PASS
