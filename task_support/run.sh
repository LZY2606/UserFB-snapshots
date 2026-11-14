#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPATH="$PWD/.task-build/gopath" GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/modcache"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s graph_decoder_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -v -count=1 -run "^(TestGraphIdentity|TestGraphRouting|TestGraphPublication)$" . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestGraphIdentity TestGraphRouting TestGraphPublication; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash baseline/run.sh
