#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]] || exit 2
task_build=$(mktemp -d "${TMPDIR:-/tmp}/split-dwp.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
export GOMODCACHE="$task_build/mod" GOCACHE="$task_build/cache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off
mkdir -p "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s elf/dwarf_package.go
  test -s elf/split_functions.go
  test -s elf/split_attribution_contract_test.go
  output_log=$(mktemp)
  trap 'rm -rf "$task_build"; rm -f "$output_log"' EXIT
  set +e
  go test -count=1 -v ./elf -run '^(TestSplitDWPIndex|TestSplitFunctions|TestSplitAttribution)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSplitDWPIndex TestSplitFunctions TestSplitAttribution; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
  test -s examples/split-attribution/main.go
  go build -o "$task_build/split-attribution" ./examples/split-attribution
  mkdir -p reports
  for sample in primary alternate; do
    sample_dir="task_support/samples/$sample"
    "$task_build/split-attribution" -image "$sample_dir/device.elf" -dwp "$sample_dir/device.elf.dwp" -queries "$sample_dir/queries.json" > "reports/$sample.json"
    go run ./task_support/check_report.go "$sample_dir/queries.json" "reports/$sample.json"
  done
fi
go run ./task_support/baseline
go test -count=1 ./elf -run '^(TestOpen|TestDWARFRelocations|TestCompressedDWARF)$'
