#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor GOSUMDB=off
go build ./...
if [[ $mode == target ]]; then
  test -s webvtt_cascade_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v . -run '^(TestCueCascade|TestCueCascadeEdits|TestCueCascadeExport)$' -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCueCascade TestCueCascadeEdits TestCueCascadeExport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline.go; else go test . -run '^(TestWebVTT|TestWebVTTTags|TestWebVTTColorToTTML|TestTTML|TestTTMLBreakLines|TestWriteToTTMLWithIndentOption|TestTTMLMergeStyleAttributes)$' -count=1; fi
