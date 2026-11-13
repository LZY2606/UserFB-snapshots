#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
export GOMODCACHE="$cache/mod" GOCACHE="$cache/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
if [[ ${1:-baseline} == baseline ]]; then
 go test -mod=vendor ./pkg/dwarf/line -run '^TestBaseline$' -count=1 -v
else
 test -s pkg/dwarf/line/source_files.go
 test -s pkg/dwarf/line/source_files_test.go
 go test -mod=vendor ./pkg/dwarf/... -count=1 -v | tee "$cache/tests.log"
 for name in forms identity dynamic compatibility; do
  grep -q -- "--- PASS: TestSourceFiles/$name " "$cache/tests.log"
 done
 if grep -q -- '--- SKIP:' "$cache/tests.log"; then exit 1; fi
fi
