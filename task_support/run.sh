#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/modules" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
if [[ $mode == target ]]; then
  test -s core/downloader/file_target_test.go
  output_log=$(mktemp)
  trap 'rm -rf "$task_cache"; rm -f "$output_log"' EXIT
  set +e
  go test -vet=off -count=1 -timeout=120s -v -run '^(TestFileParallel|TestFileResume|TestFilePublish)$' ./core/downloader >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFileParallel TestFileResume TestFilePublish; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
go test -vet=off -count=1 -timeout=30s ./core/scheduler ./core/common/resource_manage
