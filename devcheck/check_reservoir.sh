#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for f in tests/reservoir.c examples/reservoir.c; do
  if ! test -s "$f"; then printf 'MISSING_TARGET %s\n' "$f"; exit 2; fi
done
bash devcheck/run_baseline.sh
bash devcheck/legacy_cli.sh
d=$(mktemp -d "${TMPDIR:-/tmp}/reservoir.XXXXXX")
trap 'rm -rf "$d"' EXIT
"${CC:-cc}" -std=c99 -O2 -fsigned-char -Isrc/lib -Idevcheck src/lib/*.c tests/reservoir.c -lm -o "$d/reservoir"
"$d/reservoir" | tee "$d/report"
for name in legacy borrow layout streaming versions; do
  test "$(grep -Fxc "RESERVOIR_CASE $name PASS" "$d/report" || true)" = 1
done
test "$(grep -Ec '^RESERVOIR_CASE [a-z]+ PASS$' "$d/report" || true)" = 5
grep -Fxq 'SHINE_RESERVOIR_OK cases=5' "$d/report"
"${CC:-cc}" -std=c99 -O2 -fsigned-char -Isrc/lib src/lib/*.c examples/reservoir.c -lm -o "$d/example"
"$d/example" "$d/example.mp3"
test -s "$d/example.mp3"
.build/decode "$d/example.mp3"
