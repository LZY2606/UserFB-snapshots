#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/cache .task-build/mod
export GOCACHE="$PWD/.task-build/cache" GOMODCACHE="$PWD/.task-build/mod" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
export GOFLAGS=-mod=readonly
go test -count=1 ./task_support ./task_support/fixture
if [[ $mode == target ]]; then
  test -s typeunit5_regression_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -timeout=40s -v . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTypeUnit5Coordinates TestTypeUnit5RecursiveIdentity TestTypeUnit5LegacyIsolation; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./cmd/baseline
