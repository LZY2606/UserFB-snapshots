#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_dir=$(mktemp -d "${TMPDIR:-/tmp}/readability-trace.XXXXXX")
trap 'rm -rf "$cache_dir"' EXIT
export GOCACHE="$cache_dir/build" GOMODCACHE="$cache_dir/mod" GOPROXY=off GOSUMDB=off GOWORK=off GOTOOLCHAIN=local
if [[ $mode == baseline ]]; then
 go test -mod=vendor -count=1 -v -run '^TestSelectionBaseline$' .
elif [[ $mode == target ]]; then
 test -s selection_trace_test.go
 set +e
 go test -mod=vendor -count=1 -v -run '^TestSelectionTrace(Scores|Identity|Attempts)$' . >"$cache_dir/output" 2>&1
 result=$?
 set -e
 cat "$cache_dir/output"
 [[ $result == 0 ]] || exit "$result"
 if grep -Eq '^[[:space:]]*--- SKIP:' "$cache_dir/output"; then exit 3; fi
 for group in TestSelectionTraceScores TestSelectionTraceIdentity TestSelectionTraceAttempts; do
  grep -Eq "^=== RUN +${group}$" "$cache_dir/output"
  grep -Eq "^--- PASS: ${group} " "$cache_dir/output"
  grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$cache_dir/output"
 done
 grep -qx 'PASS' "$cache_dir/output"
 go test -mod=vendor -count=1 -run '^(Test_parser|Test_indexOf|Test_wordCount|Test_toAbsoluteURI)$' .
else exit 2; fi
