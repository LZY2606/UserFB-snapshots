#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_tmp=$(mktemp -d)
mkdir -p "$task_tmp/modcache" "$task_tmp/gocache" "$task_tmp/build"
export GOMODCACHE="$task_tmp/modcache" GOCACHE="$task_tmp/gocache" GOTMPDIR="$task_tmp/build"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor GOWORK=off GOENV=off
cleanup_task() { rm -rf "$task_tmp"; if [[ -n ${output_log:-} ]]; then rm -f "$output_log"; fi; }
trap cleanup_task EXIT
run_target() {
  trap cleanup_task EXIT
  go test -v -count=1 -timeout=30s ./imapclient -run '^(TestQResyncNegotiation|TestQResyncMailboxBoundary|TestQResyncDeltaCollection)$'
}
run_baseline() {
  trap cleanup_task EXIT
  go test -v -count=1 -timeout=30s ./imapclient -run '^(TestCondstoreCollection|TestSelect|TestSelectReadOnly|TestUnselectAndExpungeReadOnly|TestFetch|TestFetchPartialOffsetEcho|TestExpunge|TestUIDExpunge)$'
}
if [[ $mode == target ]]; then
  test -s imapclient/qresync_collection_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  run_target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestQResyncNegotiation TestQResyncMailboxBoundary TestQResyncDeltaCollection; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
run_baseline
