#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in glslang/MachineIndependent/constantArgumentSpecialization.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s Test/constant_argument_specialization.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --target task_baseline glslang-standalone -j 4
if [[ $mode == target ]]; then
  find .task-build/CMakeFiles/task_target.dir -type f -name '*.o' -delete
cmake --build .task-build --target task_target -j 4
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in binding ownership transitive budget; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/upstream/StandAlone/glslang -V --auto-map-locations Test/spv.functionCall.frag -o .task-build/upstream-function.spv
test -s .task-build/upstream-function.spv
echo UPSTREAM_FUNCTION_OK
