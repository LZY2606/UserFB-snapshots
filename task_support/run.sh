#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache_dir=$(mktemp -d); trap 'rm -rf "$task_cache_dir"' EXIT; export GOMODCACHE="$task_cache_dir/mod" GOCACHE="$task_cache_dir/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local; export GOFLAGS=-mod=readonly; go build ./...
if [[ $mode == target ]]; then
  test -s subscriptions_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -count=1 -v . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSubscriptionAdmission TestSubscriptionDispatch TestSubscriptionTermination; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
