#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=readonly GOENV=off GOWORK=off
task_tmp=$(mktemp -d "$PWD/.task-tmp.XXXXXX")
mkdir -p "$task_tmp/build" "$task_tmp/mod"
export GOCACHE="$task_tmp/build" GOMODCACHE="$task_tmp/mod" TMPDIR="$task_tmp"
cleanup() { rm -rf "$task_tmp"; }
trap cleanup EXIT
if [[ $mode == target ]]; then
  test -s pkg/dwarf/op/location_write_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; cleanup' EXIT
  set +e
  go test -v -count=1 -run '^(TestLocationWriteAliases|TestLocationWritePreflight|TestLocationWriteCompatibility)$' ./pkg/dwarf/op >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestLocationWriteAliases TestLocationWritePreflight TestLocationWriteCompatibility; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 -run "^(TestExecuteStackProgram|TestSignExtension|TestStackOps|TestBinaryOp_ShrVsShra_NegativeShiftedValue|TestBra)$" ./pkg/dwarf/op
go test -v -count=1 ./pkg/dwarf/leb128 ./pkg/dwarf
go run ./task_support/baseline.go
