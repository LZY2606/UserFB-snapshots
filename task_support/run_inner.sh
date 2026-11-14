#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS="-mod=vendor" GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/go-mod"
mkdir -p "$GOCACHE" "$GOMODCACHE"
if [[ $mode == target ]]; then
  test -s warc_handler_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -v -count=1 -run '^(TestWARCRecords|TestWARCDedup|TestWARCFailure)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestWARCRecords TestWARCDedup TestWARCFailure; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
go test -race -v -count=1 -run '^(TestNew|TestSendVariadic|TestCustomCommand|TestMux|TestBaseline)$' .
