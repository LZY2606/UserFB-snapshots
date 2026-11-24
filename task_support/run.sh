#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
[[ "$mode" == baseline || "$mode" == target ]]
if [[ $mode == target ]]; then
  test -s experimental/memanalysis/contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  { go list -f '{{join .XTestGoFiles "\n"}}{{"\n"}}{{join .TestGoFiles "\n"}}' ./experimental/memanalysis | grep -qx contract_test.go && go test -buildvcs=false -count=1 -timeout 60s -v -run '^(TestObjectBoundary|TestWriteOrigins|TestCrossCallEvidence|TestAnalysisLifecycle)$' ./experimental/memanalysis && build_dir=$(mktemp -d) && go build -buildvcs=false -o "$build_dir/memanalysis" ./examples/memanalysis; build_rc=$?; if [[ -n ${build_dir:-} ]]; then rm -rf "$build_dir"; fi; (exit "$build_rc"); } >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestObjectBoundary TestWriteOrigins TestCrossCallEvidence TestAnalysisLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash baseline/run.sh
