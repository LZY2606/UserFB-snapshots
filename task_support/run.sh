#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/thumb_ir_cases.c; fi
cmake -S . -B .task-build -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM_SUPPORT=ON -DCAPSTONE_BUILD_TESTS=ON -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_BUILD_MACOS_THIN=ON -DBUILD_SHARED_LIBS=OFF >.task-build-config.log
cmake --build .task-build --parallel 2 >.task-build-build.log
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude task_support/driver.c tests/thumb_ir_cases.c .task-build/libcapstone.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in flags predication memory; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/test_arm >.task-build/old-test-arm.log
printf 'OLD_TEST_ARM_OK\n'
cc -std=c99 -Iinclude task_support/witness.c .task-build/libcapstone.a -o .task-build/witness
.task-build/witness
