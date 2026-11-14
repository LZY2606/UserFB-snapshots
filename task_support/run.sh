#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/cache .task-build/mod .task-build/path
export GOCACHE="$PWD/.task-build/cache" GOMODCACHE="$PWD/.task-build/mod" GOPATH="$PWD/.task-build/path"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
if [[ $mode == target ]]; then
  test -s digest_auth_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -v -count=1 -timeout 90s -run '^(TestDigestChallenge|TestDigestReplay|TestDigestSessions)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestDigestChallenge TestDigestReplay TestDigestSessions; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/baseline.sh
