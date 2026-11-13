#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-go.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT
mkdir -p "$task_cache/mod" "$task_cache/build"
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build"
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
export HTTP_PROXY=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 ALL_PROXY=http://127.0.0.1:9
export http_proxy="$HTTP_PROXY" https_proxy="$HTTPS_PROXY" all_proxy="$ALL_PROXY" NO_PROXY= no_proxy=
[[ $mode == baseline || $mode == target ]] || { echo 'use baseline or target' >&2; exit 2; }
if [[ $mode == target ]]; then
  test -s ruby_test.go || { echo "missing nonempty ruby_test.go" >&2; exit 2; }
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  go test -mod=vendor -count=1 -v -run '^TestRuby(WebVTT|TTML|Edit)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  if grep -Eq "^[[:space:]]*--- SKIP:" "$output_log"; then exit 4; fi
  for group in TestRubyWebVTT TestRubyTTML TestRubyEdit; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -mod=vendor -count=1 -run '^(TestWebVTT|TestWebVTTTags|TestWebVTTColorToTTML|TestTTML|TestTTMLBreakLines|TestSRT|TestSRTStyled)$' .
go run -mod=vendor ./task_support/baseline
