#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOFLAGS=-mod=vendor
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -r "$cache"' EXIT
export GOMODCACHE="$cache/mod" GOCACHE="$cache/build"
go run ./checks/frame_baseline.go
go test -count=1 -v ./pkg/dwarf/frame | tee "$cache/old.log"
grep -E -- '--- PASS: TestExecuteDwarfProgram' "$cache/old.log" >/dev/null
if [[ ${1:-baseline} == target ]]; then
 test -s pkg/dwarf/frame/formats_regression_test.go
 go test -count=1 -v ./pkg/dwarf/frame -run '^TestFrameFormats$' | tee "$cache/target.log"
 grep -F -- '--- PASS: TestFrameFormats ' "$cache/target.log" >/dev/null
 for n in debug32_be debug64_le debug64_be mixed_cie address_width eh_compat malformed; do
  grep -F -- "--- PASS: TestFrameFormats/$n " "$cache/target.log" >/dev/null
 done
 printf 'FRAME_FORMATS_OK cases=7\n'
fi
