#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/gsm-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
for GSM_OPT in 0 2; do
 for GSM_NO_SASR in 0 1; do
  export GSM_OPT GSM_NO_SASR
  GSM_TARGET=1 build_case tests/portable_fixed.c devcheck/target_main.c
  "$b/case" > "$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
test "$(grep -Ec '^TARGET_CASE arithmetic assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE stream assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Ec '^TARGET_CASE parameters assertions=[1-9][0-9]* PASS$' "$b/result")" = 1
test "$(grep -Fxc 'TARGET_OK groups=3' "$b/result")" = 1
  echo "GSM_BUILD O=$GSM_OPT portable_shift=$GSM_NO_SASR PASS"
 done
done
GSM_NO_SASR=0 GSM_OPT=2 GSM_TARGET=0 bash devcheck/baseline.sh
