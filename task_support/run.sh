#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOPATH="$PWD/.task-cache/gopath" GOMODCACHE="$PWD/.task-cache/mod" GOCACHE="$PWD/.task-cache/build"
mkdir -p "$GOPATH" "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s collection_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -count=1 -v -run '^(TestCollectionIdentity|TestCollectionRebuild|TestCollectionPublication)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCollectionIdentity TestCollectionRebuild TestCollectionPublication; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/historical
go run -mod=vendor ./task_support/collection-baseline
go test -mod=vendor -count=1 . ./rss ./atom ./json
