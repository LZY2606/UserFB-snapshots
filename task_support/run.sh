#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/mod" GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
if [[ $mode == target ]]; then
  test -s bidi_proxy.go
  test -s bidi_proxy_test.go
  output_log=$(mktemp)
  trap 'rm -rf "$task_cache"; rm -f "$output_log"' EXIT
  set +e
  go test . -race -run '^TestBiDi(RoutePublication|TunnelRelay|SessionRetirement)$' -v -count=1 -timeout=90s >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestBiDiRoutePublication TestBiDiTunnelRelay TestBiDiSessionRetirement; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test . -run '^TestBaseline$' -v -count=1 -timeout=60s
go test ./... -count=1 -timeout=60s
