#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
ANT_TASK_CACHE=$(mktemp -d)
trap 'rm -rf "$ANT_TASK_CACHE"' EXIT
export GOMODCACHE="$ANT_TASK_CACHE/mod" GOCACHE="$ANT_TASK_CACHE/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
bash task_support/install.sh
if [[ $mode == target ]]; then
  test -s refresh_regression_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$ANT_TASK_CACHE"' EXIT
  set +e
  go test -mod=vendor -count=1 -timeout=75s -v . -run '^(TestRefreshMailbox|TestRefreshRepresentations|TestRefreshDrain)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRefreshMailbox TestRefreshRepresentations TestRefreshDrain; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
