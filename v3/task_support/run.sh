#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-cache.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
go test -count=1 . -run '^(TestResponseOpen|TestResponseBytes|TestAutoResume|TestNoStore|TestBeforeCopyHook|TestAfterCopyHook|TestChecksums)$'
if [[ $mode == target ]]; then
  test -s live_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  go test -race -count=1 -v . -run '^(TestLiveVisibility|TestLiveTerminal|TestLiveIsolation)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestLiveVisibility TestLiveTerminal TestLiveIsolation; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
