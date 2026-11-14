#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
export GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/go-mod" GOPATH="$PWD/.task-build/go-path"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOPATH"

if [[ $mode == target ]]; then
  test -s internal/crawler/nofollow_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -v -count=1 -timeout=60s -run "^(TestNofollowDocument|TestNofollowPromotion|TestNofollowIsolation)$" ./internal/crawler >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestNofollowDocument TestNofollowPromotion TestNofollowIsolation; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -race -v -count=1 -timeout=60s -run "^TestBaseline" ./internal/crawler
go test -v -count=1 -timeout=60s ./internal/links ./internal/crawler
