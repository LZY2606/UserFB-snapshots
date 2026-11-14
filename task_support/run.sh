#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gocache .task-build/gopath
export GOCACHE="$PWD/.task-build/gocache" GOPATH="$PWD/.task-build/gopath" GOMODCACHE="$PWD/.task-build/gopath/pkg/mod"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=readonly
if [[ $mode == target ]]; then
  test -s task_replay_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -timeout=60s -v -run '^TestTask(ShrinkIdentity|ReplayFiles|ByteBridge)$' . -rapid.seed=1815 -rapid.nofailfile >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTaskShrinkIdentity TestTaskReplayFiles TestTaskByteBridge; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 -short -timeout=75s -run '^(TestJsfRand|TestRandomBitSteam_DrawBits|TestShrink_IntCmp|TestShrink_IntSliceElemGe|TestFailFilePattern|TestFailFileRoundtrip|TestCheckCleanup|TestCheckContext|TestPanicTraceback)$' . -rapid.seed=1815 -rapid.checks=20 -rapid.nofailfile
printf '%s\n' RAPID_BASELINE_OK
