#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
mkdir -p .task-build
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache"
go build -mod=vendor ./pkg/formats/fmp4
if [[ $mode == target ]]; then
  test -s pkg/formats/fmp4/task_recorder_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor ./pkg/formats/fmp4 -run '^(TestRecorderRecovery|TestRecorderTimeline|TestRecorderFailures)$' -v -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRecorderRecovery TestRecorderTimeline TestRecorderFailures; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi

