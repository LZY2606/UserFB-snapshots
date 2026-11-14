#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
if [[ $mode == baseline ]]; then
  exec_status=0
  go run ./task_support/baseline || exec_status=$?
  exit "$exec_status"
fi
[[ $mode == target ]]
test -s collection_contract_test.go
test -s collection.go
output_log="$task_cache/result.log"
set +e
go test -count=1 -timeout=45s -v . >"$output_log" 2>&1
status=$?
set -e
cat "$output_log"
[[ $status == 0 ]] || exit "$status"
for group in TestCollectionBarrier TestCollectionResume TestCollectionIsolation; do
  grep -Eq "^=== RUN +${group}$" "$output_log"
  grep -Eq "^--- PASS: ${group} " "$output_log"
  if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
  grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
done
grep -qx PASS "$output_log"
