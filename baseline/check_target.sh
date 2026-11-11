#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for file in include/relocate.h src/relocate.c tests/relocate_cases.c tests/run_relocate.sh tests/relocate_README.md; do
  if [[ ! -s "$file" ]] || ! grep -q '[^[:space:]]' "$file"; then
    printf 'MISSING_TARGET_FILE %s\n' "$file" >&2
    exit 90
  fi
done
output=$(mktemp "$PWD/baseline/.target-output.XXXXXX")
trap 'find "$output" -type f -delete' EXIT
bash tests/run_relocate.sh > "$output" 2>&1 || { cat "$output"; exit 92; }
cat "$output"
for name in field_spans widening_cascade far_transfers loop_flags rip_data diagnostics abi_reuse; do
  if [[ $(grep -c -x "PASS $name" "$output" || true) != 1 ]]; then
    printf 'MISSING_OR_DUPLICATE_CASE %s\n' "$name" >&2
    exit 91
  fi
done
if [[ $(grep -c '^PASS ' "$output" || true) != 7 ]] ||
   [[ $(grep -c -x 'RELOCATE_CASES_OK cases=7' "$output" || true) != 1 ]] ||
   grep -Eiq '(^|[[:space:]])(SKIP|SKIPPED|TODO|XFAIL)([[:space:]:]|$)' "$output"; then
  printf '%s\n' 'INVALID_TARGET_CASE_COUNT_OR_SKIP' >&2
  exit 91
fi
bash baseline/run.sh
printf '%s\n' 'RELOCATE_TARGET_OK'
