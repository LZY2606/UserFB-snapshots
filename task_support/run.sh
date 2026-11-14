#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
for variant in float fixed; do
 bash task_support/build.sh "$variant" "$mode"
 if [[ $mode == baseline ]]; then
  ".task-build/baseline-$variant"
 else
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  set +e
  ".task-build/target-$variant" >"$log" 2>&1
  status=$?
  set -e
  cat "$log"
  [[ $status == 0 ]] || exit "$status"
  for group in gain metadata stream; do
   grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$log"
  rm -f "$log"
  echo "DRC_VARIANT $variant COMPLETE"
 fi
done
