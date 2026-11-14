#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/Z1953-cache.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor

go test -run '^$' . ./internal/objcreader ./pkg/fixupchains
if [[ $mode == target ]]; then
  test -s threaded_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  go test -v . -run '^(TestThreadedRecords|TestThreadedConsumers|TestThreadedCompatibility|TestChainedPointerVMConversion|TestGetSlidPointerAtAddressDyldInfoOnlyRebase)$' -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestThreadedRecords TestThreadedConsumers TestThreadedCompatibility; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/cmd/baseline
