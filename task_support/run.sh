#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOENV=off GOWORK=off GOTOOLCHAIN=local GOTELEMETRY=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly
export GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/go-mod"
mkdir -p "$GOCACHE" "$GOMODCACHE"
if [[ $mode == target ]]; then
  test -s format/rtsp/task_http_tunnel_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v ./format/rtsp -run '^(TestHTTPTunnelSession|TestHTTPTunnelFraming|TestHTTPTunnelLifetime)$' -count=1 -timeout 45s >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestHTTPTunnelSession TestHTTPTunnelFraming TestHTTPTunnelLifetime; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then
go test -v -tags taskbaseline ./format/rtsp -run '^TestBaseline$' -count=1 -timeout 20s
fi
