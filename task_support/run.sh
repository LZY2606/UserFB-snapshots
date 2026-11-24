#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_run_cache=$(mktemp -d)
export GOCACHE="$task_run_cache/build" GOMODCACHE="$task_run_cache/mod" GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-mod=readonly
go test ./internal/wasm/binary -run "^Test(MemoryType|MemorySection|DecodeMemoryType_Errors|MemorySection_Errors)$"
go test ./internal/wasm -run "^TestModule_ValidateFunction_BulkMemoryOperations$"
if [[ $mode == target ]]; then
  test -s internal/wasm/binary/inspection_memory_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./internal/wasm/binary -run '^TestInspectionMemory(Identity|Typing|Offsets)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestInspectionMemoryIdentity TestInspectionMemoryTyping TestInspectionMemoryOffsets; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
rm -rf "$task_run_cache"
