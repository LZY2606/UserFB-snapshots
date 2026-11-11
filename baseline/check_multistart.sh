#!/bin/bash
set -euo pipefail
export GOMODCACHE="$PWD/.gomod"
export GOPATH="$PWD/.go"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOCACHE="$build/cache"
go test -v ./pkg/fixupchains -run '^(TestMultiStartContract|TestRebaseRuntimeOffsets)$' -count=1 2>&1 | tee "$build/result.log"
grep -q -- '--- PASS: TestMultiStartContract ' "$build/result.log"
grep -q -- '--- PASS: TestRebaseRuntimeOffsets ' "$build/result.log"
echo MULTISTART_CONTRACT_OK
