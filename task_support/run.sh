#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
export GOMODCACHE="$task_tmp/mod" GOCACHE="$PWD/.task-build/go-cache" GOPATH="$task_tmp/gopath" GOTMPDIR="$task_tmp" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off GOFLAGS=
mkdir -p "$GOCACHE"
go build -modfile=task_support/offline.mod -mod=mod . ./api
if [[ $mode == target ]]; then
  test -s task_support/origin_reset_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -modfile=task_support/offline.mod -mod=mod -count=1 -timeout=45s -v task_support/origin_reset_target_test.go >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestOriginObservation TestOriginCleanup TestOriginCleanupRecovery; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -modfile=task_support/offline.mod -mod=mod -count=1 -v task_support/baseline_test.go
