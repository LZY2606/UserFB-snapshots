#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$1
cc -std=c11 -O2 -Ichecks/platform -Iinclude -D_V_SELFTEST src/bitwise.c -o "$work/bitwise"
cc -std=c11 -O2 -Ichecks/platform -Iinclude -D_V_SELFTEST src/framing.c -o "$work/framing"
"$work/bitwise" >"$work/bitwise.log" 2>&1
"$work/framing" >"$work/framing.log" 2>&1
cat "$work/bitwise.log" "$work/framing.log"
echo 'OGG_OLD_SELFTEST bitwise=pass framing=pass'
