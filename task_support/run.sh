#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/twcc_feedback.cpp; fi
mkdir -p .task-build
if ! cmake -S . -B .task-build/cmake -DUVGRTP_DISABLE_CRYPTO=ON -DUVGRTP_DISABLE_TESTS=ON -DUVGRTP_DISABLE_EXAMPLES=ON -DUVGRTP_DISABLE_INSTALL=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON >.task-build/build.log 2>&1 || ! cmake --build .task-build/cmake --target uvgrtp -j 4 >>.task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
c++ -std=c++17 -pthread -I. -Iinclude -Isrc -I.task-build/cmake/include task_support/baseline.cpp .task-build/cmake/libuvgrtp.a -framework Security -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -pthread -I. -Iinclude -Isrc -I.task-build/cmake/include -x c++ task_support/driver.c test/twcc_feedback.cpp -x none .task-build/cmake/libuvgrtp.a -framework Security -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transport wire lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
