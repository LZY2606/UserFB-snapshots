#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/modcache .task-build/gocache
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
go build -mod=vendor ./...
if [[ $mode == target ]]; then
  test -s btf/rebase_task_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -count=1 -v -run '^(TestRebaseIdentity|TestRebaseLookup|TestRebaseCopy|TestRebaseErrors)$' ./btf >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRebaseIdentity TestRebaseLookup TestRebaseCopy TestRebaseErrors; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
