#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOTOOLCHAIN=local GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor GOMODCACHE="$PWD/.task-cache/mod" GOCACHE="$PWD/.task-cache/build"; mkdir -p "$GOMODCACHE" "$GOCACHE"; go build . ./rpcc
if [[ $mode == target ]]; then
  test -s sync_preserve_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race . ./rpcc -run '^(TestPreserveBufferedOrder|TestPreserveHandoff|TestPreserveGenerated|TestSync|TestSyncError|TestStreamSyncNotifyDeadlock|TestStreamSyncSameStreamDeadlock|TestStreamSyncClosingStreams)$' -count=1 -timeout=90s -v >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestPreserveBufferedOrder TestPreserveHandoff TestPreserveGenerated; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline; go test ./rpcc -count=1 -timeout=90s
