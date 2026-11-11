#!/usr/bin/env bash
set -euo pipefail
test -s ttml_timing_tree_test.go
o=$(mktemp)
trap 'unlink "$o"' EXIT
go test . -run '^Test(TTMLTimingTree|TTML$|TTMLBreakLines|WriteToTTMLWithIndentOption|TTMLMergeStyleAttributes)' -v -count=1 | tee "$o"
grep -Eq -- '--- PASS: TestTTMLTimingTree' "$o"
! grep -Eq -- '--- SKIP: TestTTMLTimingTree' "$o"
