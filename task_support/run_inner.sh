#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
source task_support/env.sh
if [[ $mode == target ]]; then
  test -s reception_stats_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; task_cleanup' EXIT
  set +e
  go test -v -count=1 -run '^(TestReceptionSequence|TestReceptionIntervals|TestReceptionClock)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestReceptionSequence TestReceptionIntervals TestReceptionClock; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 ./task_support
go test -v -count=1 -run '^(TestReceiverReportUnmarshal|TestReceiverReportRoundTrip|TestSenderReportUnmarshal|TestSenderReportRoundTrip|TestCompoundPacketRoundTrip|TestGoodbyeRoundTrip|TestSourceDescriptionRoundTrip)$' .
