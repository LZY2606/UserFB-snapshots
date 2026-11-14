#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache_root=$(mktemp -d)
trap 'rm -rf "$task_cache_root"' EXIT
export GOMODCACHE="$task_cache_root/mod" GOCACHE="$task_cache_root/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
mkdir -p .task-build
go build ./...
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s tests/reader_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./tests >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestReaderHeaders TestReaderSymbols TestReaderIO; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == target ]]; then go test -count=1 . ./elf_view; fi
.task-build/baseline
