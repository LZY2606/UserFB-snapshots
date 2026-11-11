#!/usr/bin/env bash
set -euo pipefail
export GOCACHE="$PWD/baseline/.gocache" GOMODCACHE="$PWD/baseline/.gomodcache" GOTELEMETRY=off
out=$(mktemp "${TMPDIR:-/tmp}/pe-baseline.XXXXXX"); trap 'rm -f "$out"' EXIT
go test -count=1 -v -run '^(TestBaseline|TestOpen|TestAddBaseAndSectionRelocations)$' ./pe | tee "$out"
for name in TestBaseline TestOpen TestAddBaseAndSectionRelocations; do grep -q -- "--- PASS: $name " "$out"; done
