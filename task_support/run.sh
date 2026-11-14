#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off GOFLAGS=-mod=vendor
export GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/go-mod"
mkdir -p "$GOCACHE" "$GOMODCACHE"
if [[ $mode == target ]]; then
  test -s crawl_protocol_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -vet=off -race -count=1 -timeout=45s -v -run '^(TestCrawlProtocolAdmission|TestCrawlProtocolHandoff|TestCrawlProtocolRuns)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCrawlProtocolAdmission TestCrawlProtocolHandoff TestCrawlProtocolRuns; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -race ./task_support/witness
go test -vet=off -race -count=1 -timeout=30s -v -run '^(TestCloneForRedirect|TestRedirectURLContext|TestHandleBaseTag|TestBaseTag)$' .
