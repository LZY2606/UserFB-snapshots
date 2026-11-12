#!/usr/bin/env bash
set -euo pipefail
test -s test/mix_preview_test.c
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -I ebur128 ebur128/ebur128.c test/mix_preview_test.c -lm -o "$b/mix"
"$b/mix" | tee "$b/target.log"
for name in covariance gating history compatibility; do
 grep -Fx "MIX_PREVIEW_CASE $name PASS" "$b/target.log"
done
cc -std=c99 -O2 -I ebur128 ebur128/ebur128.c baseline/start.c -lm -o "$b/start"
"$b/start"
printf 'MIX_PREVIEW_OK cases=4\n'
