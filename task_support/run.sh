#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in pt/fluorescence.go; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
mkdir -p .task-build
go_work=$(mktemp -d "${TMPDIR:-/tmp}/fluoro-go.XXXXXX")
trap 'rm -rf "$go_work"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOENV=off GOWORK=off CGO_ENABLED=0 GOFLAGS=
export GOCACHE="$go_work/cache" GOMODCACHE="$go_work/modcache" GOPATH="$go_work/gopath"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOPATH"
if [[ "$mode" == build ]]; then go test -count=1 -run '^$' ./pt; exit 0; fi
if [[ $mode == target ]]; then
  test -s pt/fluorescence_transport_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v -count=1 ./pt -run '^(TestFluoroKernel|TestFluoroAdjoint|TestFluoroTransport)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFluoroKernel TestFluoroAdjoint TestFluoroTransport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 ./pt -run '^TestSeed(Geometry|Transport|CameraAndBounce|RGBTransport)$'
echo BASELINE_OK
