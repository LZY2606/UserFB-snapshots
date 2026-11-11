#!/usr/bin/env bash
set -euo pipefail
test -s rust_ast_contract_test.go
export GOCACHE="$PWD/baseline/.gocache" GOMODCACHE="$PWD/baseline/.gomodcache" GOTELEMETRY=off
out=$(mktemp); trap 'rm -f "$out"' EXIT
go test -count=1 -v -run '^TestRustASTContract_' . | tee "$out"
for name in Structure Backrefs Binders Printer; do grep -q -- "--- PASS: TestRustASTContract_$name " "$out"; done
! grep -q -- '--- SKIP:' "$out"
bash baseline/run.sh
