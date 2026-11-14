#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
stream_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-go.XXXXXX")
export GOMODCACHE="$stream_cache/mod" GOCACHE="$stream_cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off
trap 'rm -rf "$stream_cache"' EXIT
if [[ $mode == target ]]; then
  test -s body_stream_contract_test.go
  test -s vendor/golang.org/x/net/html/body_stream_retention_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$stream_cache"' EXIT
  set +e
  go test -mod=vendor -count=1 -v -run '^TestBodyStream(Frontier|Reclaim|Errors|Parity)$|^Test(Text|NewDocumentFromReaderWithOptions|NewDocumentFromReaderWithOptionsAppliesToAddedHtml|AppendHtmlContext|ClonePreservesNamespaces)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestBodyStreamFrontier TestBodyStreamReclaim TestBodyStreamErrors TestBodyStreamParity; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
  go test -mod=vendor -count=1 -v -run '^TestBodyStreamRetention$' golang.org/x/net/html >"$output_log" 2>&1 || { status=$?; cat "$output_log"; exit "$status"; }
  cat "$output_log"
  group=TestBodyStreamRetention
  grep -Eq "^=== RUN +${group}$" "$output_log"
  grep -Eq "^--- PASS: ${group} " "$output_log"
  if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
  grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/stream_baseline
