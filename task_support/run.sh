#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/peer_bandwidth.c; fi
mkdir -p .task-build
if ! cmake -S . -B .task-build/cmake -DUSE_OPENSSL=OFF -DUSE_MBEDTLS=OFF -DUSE_REM=OFF -DUSE_SIP=OFF -DUSE_PCP=OFF -DUSE_BFCP=OFF -DLIBRE_BUILD_SHARED=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON >.task-build/build.log 2>&1 || ! cmake --build .task-build/cmake --target task_baseline -j 4 >>.task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
if [[ $mode == target ]]; then
  rm -f .task-build/cmake/CMakeFiles/task_target.dir/test/peer_bandwidth.c.o .task-build/cmake/CMakeFiles/task_target.dir/task_support/driver.c.o .task-build/cmake/task_target
if ! cmake --build .task-build/cmake --target task_target -j 4 >>.task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
cp .task-build/cmake/task_target .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in limits credit wire; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/task_baseline "$mode"
