#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_root=$(mktemp -d)
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOMODCACHE="$cache_root/mod" GOCACHE="$cache_root/build"
go build .
if [[ $mode == target ]]; then
  test -s pacing_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v . -run '^(TestPacedTimeline|TestPacedDelivery|TestPacedReports)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestPacedTimeline TestPacedDelivery TestPacedReports; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/probe
go test -count=1 . -run '^(TestRtpPacket|TestCloneDataPacket|TestRtcpPacket)$'
