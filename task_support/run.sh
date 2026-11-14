#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=${METHA_CHECK_CACHE:-$(mktemp -d "${TMPDIR:-/tmp}/metha-check.XXXXXX")}
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
go build ./oai ./store ./harvest
if [[ $mode == target ]]; then
  test -s harvest/refresh_task_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./harvest -run '^(TestRefreshPlan|TestRefreshVisibility|TestRefreshRecovery)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRefreshPlan TestRefreshVisibility TestRefreshRecovery; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
# TestNewHarvest performs a live example.com request; keep offline comparisons here.
go test -count=1 ./oai ./store
go test -count=1 ./harvest -skip '^TestNewHarvest$'
go run ./task_support/baseline
