#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache_root=$(mktemp -d "${TMPDIR:-/tmp}/crawley-author.XXXXXX")
trap 'rm -rf "$task_cache_root"' EXIT
mkdir -p "$task_cache_root/modules" "$task_cache_root/build" .task-build
export GOMODCACHE="$task_cache_root/modules" GOCACHE="$task_cache_root/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off GOFLAGS=-mod=vendor GOTELEMETRY=off
unset GOCACHEPROG
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s internal/crawler/canonical_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache_root"' EXIT
  set +e
  go test -count=1 -timeout=90s -v -run '^(TestCanonicalHead|TestCanonicalGraph|TestCanonicalLifecycle)$' ./internal/crawler >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCanonicalHead TestCanonicalGraph TestCanonicalLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
