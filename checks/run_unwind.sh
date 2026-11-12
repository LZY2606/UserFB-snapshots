#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -r "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$cache/mod" GOCACHE="$cache/build" CGO_ENABLED=0
go run ./checks/pe_baseline.go
go test -count=1 -v ./pe -run '^(TestOpen|TestAddBaseAndSectionRelocations)$' | tee "$cache/old.log"
grep -F -- '--- PASS: TestOpen ' "$cache/old.log" >/dev/null
grep -F -- '--- PASS: TestAddBaseAndSectionRelocations ' "$cache/old.log" >/dev/null
if [[ ${1:-baseline} == target ]]; then
 test -s pe/unwind_amd64_contract_test.go
 go test -count=1 -v ./pe -run '^TestAMD64UnwindContract$' | tee "$cache/new.log"
 for n in leaf prolog frame_base large_alloc chain machine_frame errors; do
  grep -F -- "--- PASS: TestAMD64UnwindContract/$n " "$cache/new.log" >/dev/null
 done
 printf 'AMD64_UNWIND_OK cases=7\n'
fi
