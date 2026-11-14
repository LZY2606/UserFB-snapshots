#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
source task_support/env.sh
if [[ $mode == target ]]; then
  test -s sync_collect_test.go
  target_log=.task-build/target.log
  output_log=.task-build/target-raw.$$
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v -timeout=60s "${production[@]}" sync_collect_test.go -run "^(TestSyncPages|TestSyncBodies|TestSyncRecovery)$" >"$output_log" 2>&1
  status=$?
  set -e
  cp "$output_log" "$target_log"
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSyncPages TestSyncBodies TestSyncRecovery; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    grep -Eq "^TASK_GROUP ${group} assertions=[1-9][0-9]* complete$" "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  if grep -Eq "^[[:space:]]*--- (SKIP|FAIL):" "$output_log"; then exit 4; fi
  grep -qx 'PASS' "$output_log"
fi
bash task_support/baseline.sh
