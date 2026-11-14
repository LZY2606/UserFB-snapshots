#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/session_checks.cpp; fi
cmake -S . -B .task-build/lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release > .task-build-configure.log 2>&1
cmake --build .task-build/lib -j2 > .task-build-build.log 2>&1
c++ -std=c++11 -DMODPLUG_STATIC -I src -I task_support task_support/baseline.cpp .task-build/lib/libmodplug.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -I . -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -DMODPLUG_STATIC -I . -I src -I task_support tests/session_checks.cpp .task-build/driver.o .task-build/lib/libmodplug.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in formats dsp_history lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
for preset in dry16 wet16 mono8 stereo32; do .task-build/baseline check "$preset"; done
