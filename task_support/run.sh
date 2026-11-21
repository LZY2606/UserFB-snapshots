#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOMODCACHE="$cache_root/mod" GOCACHE="$cache_root/cache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s render3d/guided_light_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s render3d/guided_light.go
test -s examples/guided_light/main.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./render3d > .task-build/render3d-files.list
grep -qx guided_light.go .task-build/render3d-files.list
go build -o .task-build/api-binding ./task_support/api_binding
go build -o .task-build/guided-demo ./examples/guided_light
rm -f .task-build/guided.png
.task-build/guided-demo -output .task-build/guided.png
test -s .task-build/guided.png
go test -count=1 -v ./render3d -run '^(TestGuidedDistribution|TestGuidedStrategies|TestGuidedRender)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestGuidedDistribution TestGuidedStrategies TestGuidedRender; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
