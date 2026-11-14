#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOWORK=off GOFLAGS=-mod=readonly
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/gomodcache"
mkdir -p "$GOCACHE" "$GOMODCACHE"
if [[ $mode == target ]]; then
  test -s form_receipt_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -count=1 -timeout=75s -v -run '^(TestFormReceiptHops|TestFormReceiptIsolation|TestFormReceiptCompletion)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFormReceiptHops TestFormReceiptIsolation TestFormReceiptCompletion; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -race -count=1 -timeout=45s -v -run '^TestDispatchBaseline$' .
echo BASELINE_OK
