#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/colr_v1_contract.c || { echo 'MISSING_TARGET tests/colr_v1_contract.c' >&2; exit 2; }
build=$(mktemp -d "${TMPDIR:-/tmp}/colr-target.XXXXXX")
trap 'rm -rf "$build"' EXIT
for mode in c99 cpp11 sanitized; do
 case "$mode" in
 c99) cc -std=c99 -O2 -DNDEBUG -I. tests/colr_v1_contract.c -lm -o "$build/contract" ;;
 cpp11) c++ -std=c++11 -x c++ -O2 -DNDEBUG -I. tests/colr_v1_contract.c -lm -o "$build/contract" ;;
 sanitized) cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/colr_v1_contract.c -lm -o "$build/contract" ;;
 esac
 for group in gradient_transform isolated_composite shared_graph legacy_and_errors; do
  "$build/contract" --case "$group" > "$build/$group.out"
  cat "$build/$group.out"
  if grep -Ei 'SKIP|TODO|NOT[ _-]IMPLEMENTED' "$build/$group.out"; then exit 3; fi
  grep -Eq "^PASS ${group} checks=[1-9][0-9]*$" "$build/$group.out" || { echo "NO_EXECUTED_CHECKS $mode/$group" >&2; exit 3; }
 done
 printf 'COLR_CONTRACT_OK mode=%s groups=4\n' "$mode"
done
test -s examples/colr_preview.c || { echo 'MISSING_PREVIEW examples/colr_preview.c' >&2; exit 2; }
cc -std=c99 -O2 -I. examples/colr_preview.c -lm -o "$build/preview"
"$build/preview" "$build/preview.ppm"
test -s "$build/preview.ppm"
test "$(head -n 1 "$build/preview.ppm")" = 'P6'
printf 'COLR_PREVIEW_OK format=P6\n'
bash colrcheck/check-old.sh
