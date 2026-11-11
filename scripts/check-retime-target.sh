#!/usr/bin/env bash
set -euo pipefail
test -s retime/shift_test.go
o=$(mktemp)
trap 'unlink "$o"' EXIT
go test ./retime -run '^TestShift' -v -count=1 | tee "$o"
grep -Eq -- '--- PASS: TestShift' "$o"
! grep -Eq -- '--- SKIP: TestShift' "$o"
go test . ./packet ./pes ./psi ./scte35 -count=1
