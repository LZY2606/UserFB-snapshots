#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
export GOCACHE="$PWD/.task-cache" GOMODCACHE="$PWD/.task-modcache"
export GOFLAGS="-mod=mod"
if [[ $mode == target ]]; then
  test -s rendition_groups_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -vet=off -count=1 -v . -run '^(TestRenditionCatalog|TestRenditionRename|TestRenditionLegacy)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRenditionCatalog TestRenditionRename TestRenditionLegacy; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/witness
go test -vet=off -count=1 .
