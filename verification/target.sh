#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/openjp2-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
test -s tests/dwt_contract.c
source verification/build.sh
cc -std=c99 -O2 -DOPJ_STATIC -I. -Isrc/lib/openjp2 -I"$b/build/src/lib/openjp2"  tests/dwt_contract.c tests/dwt_reference.c verification/driver_main.c "$b/build/bin/libopenjp2.a" -lpthread -lm -o "$b/target"
"$b/target" > "$b/result"
cat "$b/result"
test "$(grep -Fxc 'CASE_PASS coefficient-grid' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS inverse-grid' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS multilevel' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS roi-reduce' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS threads' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS codec-roundtrip' "$b/result")" = 1
grep -Eq '^EXECUTED ([1-9][0-9]*)$' "$b/result"
bash verification/mutations.sh | tee "$b/mutations"
for name in origin-parity inverse-rounding singleton; do
  grep -Fxq "MUTATION_NUMERIC_PASS $name" "$b/mutations"
done
echo TARGET_ALL_PASS
