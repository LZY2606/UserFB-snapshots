#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/codec_state_cues.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build .task-build --target baseline -j2
if [[ $mode == target ]]; then
  c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -DMATROSKA_STATIC_DEFINE -DEBML_STATIC_DEFINE -I. -Ideps/libebml -I.task-build -I.task-build/ebml test/codec_state_cues.cpp .task-build/driver.o .task-build/libtask_production.a .task-build/ebml/libebml.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in inheritance relocation readback; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
