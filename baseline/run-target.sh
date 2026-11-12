#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=local GOPATH="$PWD/.go" GOCACHE="$PWD/.build"
test -s guarded_sanitizer_test.go
out=$(mktemp)
trap 'rm -f "$out"' EXIT
go test -v . -run '^TestGuardedSanitizer(Branches|Pairs|Joins|Models)$' -count=1 | tee "$out"
for name in Branches Pairs Joins Models; do
  grep -Eq "^--- PASS: TestGuardedSanitizer${name} \\(" "$out"
done
go test -v . -run '^Test(CheckDetailedAppliesValueSpecificSanitizer|CheckDetailedSanitizerWrapperSuppressesDiagnostic|AliasedFieldStoreStillTainted)$' -count=1 | tee "$out"
for name in TestCheckDetailedAppliesValueSpecificSanitizer TestCheckDetailedSanitizerWrapperSuppressesDiagnostic TestAliasedFieldStoreStillTainted; do
  grep -F -- "--- PASS: $name (" "$out"
done
printf 'GUARDED_SANITIZER_OK\n'
