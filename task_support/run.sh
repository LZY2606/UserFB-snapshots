#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
cache_root=$(mktemp -d)
export GOMODCACHE="$cache_root/mod" GOCACHE="$cache_root/build"
trap 'rm -rf "$cache_root"' EXIT
go build -mod=vendor ./mp4
if [[ $mode == target ]]; then
  test -s mp4/continuous_clock_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -count=1 -v ./mp4 -run '^(TestContinuousClockDecoder|TestContinuousClockStream|TestContinuousClockRollback)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestContinuousClockDecoder TestContinuousClockStream TestContinuousClockRollback; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
