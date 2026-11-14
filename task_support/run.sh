#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/quilt.js || { echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY src/quilt.js" >&2; exit 1; }
  node --check src/quilt.js
  test -s test/quilt-contract.js
  node --check test/quilt-contract.js
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  node task_support/driver.cjs >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in candidates cuts periodic replay; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$output_log"
elif [[ $mode != baseline ]]; then
  echo "unknown mode: $mode" >&2
  exit 2
fi
node task_support/baseline.cjs
