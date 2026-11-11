#!/bin/bash
set -euo pipefail
export GOMODCACHE="$PWD/.gomod"
export GOPATH="$PWD/.go"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOCACHE="$build/cache"
go run ./baseline
go test -v ./pkg/fixupchains -run '^TestRebaseRuntimeOffsets$' -count=1 2>&1 | tee "$build/test.log"
grep -q -- '--- PASS: TestRebaseRuntimeOffsets ' "$build/test.log"
echo OLD_REBASE_TEST_OK
