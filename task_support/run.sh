#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/weighted_pred/contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DBUILD_KVAZAAR_BINARY=OFF -DUSE_CRYPTO=OFF -DGIT_SUBMODULE=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/cmake -j 4 > .task-build/build.log 2>&1
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c11 -O2 -I. -Isrc -Isrc/extras -Isrc/strategies task_support/driver.c tests/weighted_pred/contract.c .task-build/cmake/libkvazaar.a -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in syntax prediction pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
${CC:-cc} -std=c11 -O2 -Isrc task_support/baseline.c .task-build/cmake/libkvazaar.a -lm -pthread -o .task-build/baseline
.task-build/baseline
.task-build/cmake/tests/kvazaar_tests > .task-build/upstream-tests.log 2>&1
printf 'UPSTREAM_TESTS_COMPLETE\n' 
