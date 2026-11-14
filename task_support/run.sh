#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]]
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  java -ea -cp .task-build/classes FormHarness > "$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  grep -Eq '^FORM_CONTRACT_OK cases=[1-9][0-9]* groups=3$' "$output_log"
  for group in ownership edits copies; do
    grep -Eq "^FORM_GROUP ${group} cases=[1-9][0-9]*$" "$output_log"
  done
fi
java -ea -cp .task-build/classes StartingPoint
