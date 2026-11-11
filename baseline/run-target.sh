#!/usr/bin/env bash
set -euo pipefail
test -s pe/import_rva_contract_test.go
export GOCACHE="$PWD/baseline/.gocache" GOMODCACHE="$PWD/baseline/.gomodcache" GOTELEMETRY=off
out=$(mktemp); trap 'rm -f "$out"' EXIT
go test -count=1 -v -run '^TestImportRVAContract_' ./pe | tee "$out"
for name in SplitLayout Fallback Delay Bounds; do grep -q -- "--- PASS: TestImportRVAContract_$name " "$out"; done
! grep -q -- '--- SKIP:' "$out"
go test -count=1 -v -run '^(TestOpen|TestAddBaseAndSectionRelocations)$' ./pe
