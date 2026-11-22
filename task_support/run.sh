#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_cache_root=$(mktemp -d .task-build/cache.XXXXXX)
export GOMODCACHE="$PWD/$task_cache_root/mod" GOCACHE="$PWD/$task_cache_root/build"
mkdir -p "$GOMODCACHE" "$GOCACHE"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
if [[ $mode == target ]]; then
  test -s opus_export_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v . -run '^(TestOpusExportTables|TestOpusExportTiming|TestOpusExportErrors)$' -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestOpusExportTables TestOpusExportTiming TestOpusExportErrors; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v . -run '^(TestSeedOpusTables|TestBoxTypesOpus)$' -count=1
