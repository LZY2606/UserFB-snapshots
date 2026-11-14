#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache_dir=$(mktemp -d)
trap 'rm -rf "$task_cache_dir"' EXIT
export GOMODCACHE="$task_cache_dir/mod" GOCACHE="$task_cache_dir/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off
mkdir -p "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s history_capture_test.go
  output_log=$(mktemp)
  trap 'rm -rf "$task_cache_dir"; rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -count=1 -v . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestHistoryArchive TestHistoryIncremental TestHistoryMerge; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
