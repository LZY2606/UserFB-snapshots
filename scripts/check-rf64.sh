#!/usr/bin/env bash
set -euo pipefail
test -s rf64_test.go
out=$(mktemp)
trap 'rm -f "$out"' EXIT
go test -count=1 -json -run '^(TestRF64Read|TestRF64Metadata|TestRF64AutoPromote|TestRF64Limits)$' . | tee "$out"
awk '
 /"Action":"skip"/ { skipped=1 }
 /"Action":"pass"/ && /"Test":"TestRF64(Read|Metadata|AutoPromote|Limits)"/ { count++ }
 END { if (count != 4 || skipped) { print "RF64 targets missing or skipped" > "/dev/stderr"; exit 1 }; print "RF64_TARGETS=4 PASS" }
' "$out"
go test -count=1 ./...
