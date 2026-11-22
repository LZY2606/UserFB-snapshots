#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d)
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/mod" GOPATH="$task_cache/path" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
go build ./...
if [[ $mode == target ]]; then
  test -s aligned_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -vet=off -v . -run '^(TestAlignedClocks|TestAlignedCuts|TestAlignedRoundTrip)$' -count=1 >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestAlignedClocks TestAlignedCuts TestAlignedRoundTrip; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then
  go run ./task_support/witness
  go test -vet=off -v . -run '^(TestDecodeMediaPlaylistWithProgramDateTime|TestSetMapForMediaPlaylist|TestDiscontinuityForMediaPlaylist)$' -count=1
fi
