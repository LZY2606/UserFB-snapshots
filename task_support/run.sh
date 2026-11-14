#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gocache .task-build/modcache
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/modcache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
go test -run '^TestSeedTeletextBaseline$' -count=1 -v .
if [[ $mode == target ]]; then
  test -s teletext_stream_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -run '^TestTeletextStream(Pages|Charsets|Transport)$' -count=1 -v . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTeletextStreamPages TestTeletextStreamCharsets TestTeletextStreamTransport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -run '^(TestSeedTeletextBaseline|TestTeletextPESDataType|TestTeletextPageParse|TestParseTeletextRow|TestAppendTeletextLineItem)$' -count=1 -v .
