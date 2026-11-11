#!/usr/bin/env bash
set -euo pipefail
test -s compression_analysis_test.go
o=$(mktemp)
trap 'unlink "$o"' EXIT
go test . -run '^Test(AutoCompression|Decode$|Seek$|SkipID3v2$)' -v -count=1 | tee "$o"
grep -Eq -- '--- PASS: TestAutoCompression' "$o"
! grep -Eq -- '--- SKIP: TestAutoCompression' "$o"
go test ./frame ./meta ./internal/... -count=1
