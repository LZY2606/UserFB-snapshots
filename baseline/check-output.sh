#!/usr/bin/env bash
set -euo pipefail
group=$1
output=$2
grep -Eq "^PASS $group cases=[1-9][0-9]*$" "$output" || { echo "NO_EXECUTED_CASES $group" >&2; exit 1; }
if grep -Eiq '(^|[^a-z])(skip|skipped|todo)([^a-z]|$)' "$output"; then
  echo "SKIPPED_TARGET_CASES $group" >&2
  exit 1
fi
