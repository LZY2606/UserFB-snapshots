#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_frame_cache=${TASK_FRAME_CACHE:-$(mktemp -d)}
mkdir -p "$task_frame_cache/mod" "$task_frame_cache/build"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$task_frame_cache/mod" GOCACHE="$task_frame_cache/build"
task_frame_files=$(go list -mod=vendor -f '{{join .GoFiles " "}}' .)
go test -mod=vendor -run '^$' $task_frame_files frame_baseline_test.go >/dev/null
if [[ $mode == target ]]; then
  test -s frame_scope_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -race -count=1 -timeout=60s -v -run '^(TestFrameTargetAdoption|TestFrameQueryRouting|TestFrameProcessSwap)$' $task_frame_files frame_baseline_test.go frame_scope_test.go >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFrameTargetAdoption TestFrameQueryRouting TestFrameProcessSwap; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -mod=vendor -count=1 -timeout=20s -v -run '^TestFrameBaseline$' $task_frame_files frame_baseline_test.go
