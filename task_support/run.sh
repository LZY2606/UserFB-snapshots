#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOWORK=off GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOPATH="$PWD/.task-build/gopath" GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache"
mkdir -p "$GOPATH" "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s websub_capture_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -mod=vendor -v -count=1 -timeout=90s -run '^(TestWebSubLease|TestWebSubReconcile|TestWebSubIsolation)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestWebSubLease TestWebSubReconcile TestWebSubIsolation; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
go run -mod=vendor ./task_support/linkbaseline
go test -mod=vendor -count=1 -timeout=120s -skip '^(TestWebSubLease|TestWebSubReconcile|TestWebSubIsolation)$' ./...
