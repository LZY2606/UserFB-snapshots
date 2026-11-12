#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/uf-go.XXXXXX")
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOMODCACHE="$build/modcache" GOCACHE="$build/buildcache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
go test -count=1 -v -run '^(TestSmallCopy|TestFramingFormat|TestWriterReset|TestEncodeGoldenInput)$' .
go run support/baseline.go
if [[ ${1:-baseline} == target ]]; then
 for mode in native noasm; do
 tagflag=""
 if [[ $mode == noasm ]]; then tagflag="-tags noasm"; fi
 go test $tagflag -count=1 -v -run '^(TestUnitOptimal|TestUnitBoundaries|TestUnitStream)$' . > "$build/target.log" 2>&1 || { cat "$build/target.log"; exit 1; }
 cat "$build/target.log"
 if grep -q -- '--- SKIP:' "$build/target.log"; then exit 2; fi
 grep -q -- "--- PASS: TestUnitOptimal (" "$build/target.log"
 grep -q -- "--- PASS: TestUnitOptimal/" "$build/target.log"
 grep -q -- "--- PASS: TestUnitBoundaries (" "$build/target.log"
 grep -q -- "--- PASS: TestUnitBoundaries/" "$build/target.log"
 grep -q -- "--- PASS: TestUnitStream (" "$build/target.log"
 grep -q -- "--- PASS: TestUnitStream/" "$build/target.log"
 echo "TARGET COMPLETE mode=$mode groups=3"
 done
fi
