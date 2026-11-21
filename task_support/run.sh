#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == baseline ]]; then
  node task_support/baseline.cjs
elif [[ $mode == target ]]; then
  node task_support/build.cjs
  target=$(node -p 'require("./task_support/config.json").target')
  test -s "$target"
  log=$(mktemp)
  coverage_dir=$(mktemp -d)
  trap 'rm -f "$log"; rm -rf "$coverage_dir"' EXIT
  set +e
  NODE_V8_COVERAGE="$coverage_dir" node --test --test-reporter=tap "$target" >"$log" 2>&1
  status=$?
  set -e
  cat "$log"
  [[ $status == 0 ]] || exit "$status"
  node task_support/verify.cjs "$log" "$coverage_dir"
else
  exit 2
fi
