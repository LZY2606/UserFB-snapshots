#!/usr/bin/env bash
set -euo pipefail
export GOCACHE="$PWD/baseline/.gocache" GOMODCACHE="$PWD/baseline/.gomodcache" GOTELEMETRY=off
out=$(mktemp); trap 'rm -f "$out"' EXIT
go test -count=1 -v -run '^(TestBaseline|TestRustExpected|TestRustCheck|TestRustNoTemplaraParams|TestMaxLength)$' . | tee "$out"
for name in TestBaseline TestRustExpected TestRustCheck TestRustNoTemplaraParams TestMaxLength; do grep -q -- "--- PASS: $name " "$out"; done
