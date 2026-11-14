#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
export GOFLAGS=-modfile=task_support/offline.mod
task_cache=$(mktemp -d)
export GOMODCACHE="$task_cache/modules" GOCACHE="$task_cache/build"
trap 'rm -rf "$task_cache"' EXIT
if [[ $mode == target ]]; then
  test -s obj/loadbias_task_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./obj -run '^(TestLoadBiasCoordinates|TestLoadBiasRelocations|TestLoadBiasTransitions)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestLoadBiasCoordinates TestLoadBiasRelocations TestLoadBiasTransitions; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 ./obj
go run ./task_support/baseline
