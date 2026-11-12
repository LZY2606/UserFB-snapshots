#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cp src/eia608_from_utf8.c.cached "$b/eia608_from_utf8.c"
test -s unit_tests/test_popon_encode.c
cc -std=c99 -O2 -I. -Icaption src/caption.c src/cea708.c src/eia608.c src/eia608_charmap.c "$b/eia608_from_utf8.c" src/mpeg.c src/scc.c src/srt.c src/utf8.c src/vtt.c src/xds.c unit_tests/test_popon_encode.c verification/driver_main.c -o "$b/target"
"$b/target" > "$b/result"
cat "$b/result"
test "$(grep -Fxc 'CASE_PASS style-boundary' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS sparse-grid' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS charsets' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS packet-boundary' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS wire-midrow' "$b/result")" = 1
test "$(grep -Fxc 'CASE_PASS reuse' "$b/result")" = 1
grep -Eq '^EXECUTED ([1-9][0-9]*)$' "$b/result"
echo TARGET_ALL_PASS
