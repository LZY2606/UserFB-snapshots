#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache=$(mktemp -d "${TMPDIR:-/tmp}/z2026-go.XXXXXX")
export GOMODCACHE="$cache/mod" GOCACHE="$cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
trap 'rm -r "$cache"' EXIT
if [[ $mode == target ]]; then
  test -s coalesce_target_test.go
  output_log=$(mktemp)
  trap 'rm -r "$cache"; rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -race -count=1 -timeout=45s -v -run '^TestCoalesce' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCoalesceAdmission TestCoalesceBodies TestCoalesceLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run -mod=vendor ./task_support/baseline; fi
