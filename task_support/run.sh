#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build"
go build -mod=vendor . ./queue
if [[ $mode == target ]]; then
  test -s queue/async_lifecycle_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -count=1 -timeout 100s -v -run '^(TestQueueOwnCompletion|TestQueueCallbackDiscovery|TestQueueStopResume)$' ./queue >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestQueueOwnCompletion TestQueueCallbackDiscovery TestQueueStopResume; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run -mod=vendor ./task_support/baseline; fi
