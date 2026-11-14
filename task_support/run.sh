#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target && ! -s managed_downloads.go ]]; then
  echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY managed_downloads.go" >&2
  exit 1
fi
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
export GOMODCACHE="$task_tmp/mod" GOPATH="$task_tmp/gopath" GOCACHE="$PWD/.task-build/go-cache" GOTMPDIR="$task_tmp" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off GOFLAGS=
mkdir -p "$GOCACHE"

if [[ $mode == target ]]; then
  test -s managed_downloads_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_tmp"' EXIT
  set +e
  go build -modfile=task_support/offline.mod -mod=mod . && go test -modfile=task_support/offline.mod -mod=mod -count=1 -v -run '^(TestDownloadSnapshotWire|TestDownloadSnapshotArchive|TestDownloadSnapshotPublication)$' managed_downloads_test.go >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestDownloadSnapshotWire TestDownloadSnapshotArchive TestDownloadSnapshotPublication; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -modfile=task_support/offline.mod -mod=mod -count=1 -v baseline_test.go export_baseline_test.go
go test -modfile=task_support/offline.mod -mod=mod -count=1 -v ./chrome
