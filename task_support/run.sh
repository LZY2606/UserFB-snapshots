#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in src/pmp/algorithms/intrinsic_transfer.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/intrinsic_transfer_contract.cpp; fi
transfer_target=OFF
if [[ $mode == target ]]; then transfer_target=ON; fi
cmake -S sanity -B .task-build -DCMAKE_BUILD_TYPE=Release -DTRANSFER_TARGET="$transfer_target"
cmake --build .task-build --target baseline metric_baseline old_tests --parallel 2
if [[ $mode == target ]]; then
  if [[ -d .task-build/CMakeFiles/transfer_target.dir ]]; then
  find .task-build/CMakeFiles/transfer_target.dir -name '*.o' -delete
fi
cmake --build .task-build --target transfer_target --parallel 2
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in metric projection history state; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
 .task-build/baseline
.task-build/metric_baseline
.task-build/old_tests --gtest_color=no --gtest_filter='FairingTest.*' | tee .task-build/old-tests.log
grep -Fq '[  PASSED  ] 2 tests.' .task-build/old-tests.log
! grep -Fq '[  SKIPPED ]' .task-build/old-tests.log
echo PMP_TRANSFER_BASELINE_OK
