#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s test/api/frame_lease.cpp
source verification/build.sh
c++ -std=c++17 -O2 -I. -Icodec/api/wels  test/api/frame_lease.cpp verification/driver_main.cpp "$b/libopenh264.a" -lpthread -o "$b/target"
"$b/target" > "$b/result"
cat "$b/result"
test "$(grep -Fxc 'CASE_PASS pin-reuse' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS resolution' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS destroy-owner' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS drain-thread' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS empty-output' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS release-order' "$b/result")" = 1
grep -Eq '^EXECUTED ([1-9][0-9]*)$' "$b/result"
echo TARGET_ALL_PASS
