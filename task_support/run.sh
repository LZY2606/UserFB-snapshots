#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_nav_cache=$(mktemp -d)
trap 'rm -rf "$task_nav_cache"' EXIT
export GOCACHE="$task_nav_cache/build" GOMODCACHE="$task_nav_cache/mod" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
mkdir -p "$GOCACHE" "$GOMODCACHE"
go build .
if [[ $mode == target ]]; then
  test -s navigation_wait_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -count=1 -timeout=60s -v . -run '^(TestNavigationLoader|TestNavigationRequest|TestNavigationObservers)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestNavigationLoader TestNavigationRequest TestNavigationObservers; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 -timeout=30s -v . -run '^TestNavigationBaseline$'
