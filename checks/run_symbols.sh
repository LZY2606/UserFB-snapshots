#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -r "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$cache/mod" GOCACHE="$cache/build"
go run ./checks/symbol_baseline.go
go test -count=1 -v . -run '^(TestDemangler|TestFailure|TestASTToString)$' | tee "$cache/old.log"
grep -F -- '--- PASS: TestDemangler ' "$cache/old.log" >/dev/null
if [[ ${1:-baseline} == target ]]; then
 test -s itanium_model_test.go && test -s checks/substitution_mutations.sh
 go test -count=1 -v . -run '^(TestItaniumSubstitutionModel|FuzzItaniumSubstitution)$' | tee "$cache/new.log"
 for n in prefix_chain qualified_type templates declarators candidate_order reuse; do
  grep -F -- "--- PASS: TestItaniumSubstitutionModel/$n " "$cache/new.log" >/dev/null
 done
 seeds=$(grep -c -- '--- PASS: FuzzItaniumSubstitution/seed#' "$cache/new.log" || true)
 test "$seeds" -ge 4
 bash checks/substitution_mutations.sh | tee "$cache/mut.log"
 grep -Fx 'KILLED prefix_registration' "$cache/mut.log" >/dev/null
 grep -Fx 'KILLED subst_index' "$cache/mut.log" >/dev/null
 printf 'ITANIUM_SUBSTITUTIONS_OK cases=6 mutations=2\n'
fi
