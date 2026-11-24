#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/retime_test.cpp; fi
cmake -S . -B .task-build -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DOTIO_PYTHON_INSTALL=OFF -DOTIO_AUTOMATIC_SUBMODULES=OFF -DOTIO_CXX_INSTALL=OFF -DOTIO_INSTALL_COMMANDLINE_TOOLS=OFF -DBUILD_TESTING=OFF -DIMATH_BUILD_TESTS=OFF -DIMATH_INSTALL=OFF -DOTIO_SHARED_LIBS=OFF -DMZ_ZLIB=OFF -DMZ_FETCH_LIBS=OFF
cmake --build .task-build --target task_baseline -j 4
if [[ $mode == target ]]; then
  test -s src/opentimelineio/retimedClip.cpp
cmake --build .task-build --target task_target -j 4
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in duration mapping editing_json; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_support/task_baseline
