#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
if [[ $mode == target ]]; then
  test -s crawl_pause_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  bash task_support/go.sh test -vet=off -mod=vendor -race -count=1 -timeout=45s -v -run '^(TestCrawlPauseBarrier|TestCrawlPauseContinuation|TestCrawlPauseLifecycle)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCrawlPauseBarrier TestCrawlPauseContinuation TestCrawlPauseLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/go.sh test -vet=off -mod=vendor -race -count=1 -timeout=30s -v -run '^(TestRedirectURLContext|TestHandleBaseTag|TestBaseTag)$' .
if [[ $mode != target ]]; then bash task_support/go.sh run -mod=vendor ./task_support/witness; fi
