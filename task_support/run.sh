#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]] || exit 2
if [[ $mode == target ]]; then test -s capture/collector_target_test.go; fi
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOMODCACHE="$cache_root/mod" GOCACHE="$cache_root/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
go build -mod=vendor ./rpcc ./protocol/fetch ./protocol/io
if [[ $mode == target ]]; then
  test -s capture/collector_target_test.go
  output_log=$(mktemp)
  trap 'rm -rf "$cache_root"; rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -race -count=1 -timeout=90s -v ./capture ./rpcc ./protocol/io >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCaptureReader TestCapturePublish TestCaptureShutdown; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
