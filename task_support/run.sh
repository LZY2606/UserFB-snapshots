#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
SITEMAP_TASK_CACHE=$(mktemp -d)
trap 'rm -rf "$SITEMAP_TASK_CACHE"' EXIT
export GOMODCACHE="$SITEMAP_TASK_CACHE/mod" GOCACHE="$SITEMAP_TASK_CACHE/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
bash task_support/install.sh
if [[ $mode == target ]]; then
  test -s session_graph_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$SITEMAP_TASK_CACHE"' EXIT
  set +e
  go test -mod=readonly -count=1 -timeout=75s -v . -run '^(TestSessionEvidence|TestSessionMembership|TestSessionFailure)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSessionEvidence TestSessionMembership TestSessionFailure; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=readonly ./task_support/baseline
