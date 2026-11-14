#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly
TASK_CACHE=$(mktemp -d)
trap 'rm -rf "$TASK_CACHE"' EXIT
export GOMODCACHE="$TASK_CACHE/mod" GOCACHE="$TASK_CACHE/build"
go test ./pkg/rtsp ./pkg/rtprtcp -run '^Test(AvPacketQueue|AvcCase1|SubSeq)$' -count=1
if [[ $mode == target ]]; then
  test -s pkg/rtsp/sender_report_clock_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$TASK_CACHE"' EXIT
  set +e
  go test -v ./pkg/rtsp -run '^TestSenderReport(Alignment|ClockPackets|ClockIngress)$' -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSenderReportAlignment TestSenderReportClockPackets TestSenderReportClockIngress; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
