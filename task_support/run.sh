#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_screencast_cache=${TASK_SCREENCAST_CACHE:-$(mktemp -d)}
mkdir -p "$task_screencast_cache/mod" "$task_screencast_cache/build"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$task_screencast_cache/mod" GOCACHE="$task_screencast_cache/build"
go test -run '^$' . >/dev/null
if [[ $mode == target ]]; then
  test -s screencast_flow_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -count=1 -timeout=45s -v -run '^(TestScreencastStart|TestScreencastFlow|TestScreencastStop)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestScreencastStart TestScreencastFlow TestScreencastStop; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 -timeout=20s -v -run '^TestScreencastBaseline$' .
