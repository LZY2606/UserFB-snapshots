#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/gmc_contract.c; fi
mkdir -p .task-build
cmake -S task_support -B .task-build/cmake > .task-build/configure.log 2>&1
cmake --build .task-build/cmake -j 4 > .task-build/build.log 2>&1
if [[ $mode == target ]]; then
  ${CC:-cc} -std=gnu99 -O2 -DARCH_IS_GENERIC -DARCH_IS_64BIT -DARCH_IS_LITTLE_ENDIAN -I. task_support/driver.c tests/gmc_contract.c .task-build/cmake/libtaskxvid.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry state stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
${CC:-cc} -std=c99 -O2 -I. task_support/baseline.c .task-build/cmake/libtaskxvid.a -lm -o .task-build/baseline
.task-build/baseline
