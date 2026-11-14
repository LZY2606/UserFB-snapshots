#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gocache .task-build/gomodcache
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/gomodcache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOTELEMETRY=off
if [[ $mode == target ]]; then
  test -s macho/bindings_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v -count=1 -run '^(TestBindingContract_State|TestBindingContract_Stride|TestBindingContract_Lazy|TestBindingContract_Mapping)$' ./macho >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestBindingContract_State TestBindingContract_Stride TestBindingContract_Lazy TestBindingContract_Mapping; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 -run '^(TestMachOBaseline|TestOpen|TestDyldBindOrdinal|TestScatteredRelocationRoundTrip)$' ./macho
