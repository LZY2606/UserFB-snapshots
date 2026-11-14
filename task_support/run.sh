#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in Cpp/Source/Subsurface.h Cpp/Source/Subsurface.cpp; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/subsurface_transport.cpp; fi
mkdir -p .task-build
objects=()
for source in Cpp/Source/*.cpp Cpp/Source/enkiTS/TaskScheduler.cpp Cpp/Source/enkiTS/TaskScheduler_c.cpp; do
 object=".task-build/$(basename "${source%.cpp}").o"
 "${CXX:-c++}" -std=c++17 -O2 -pthread -I. -ICpp/Source -include cstring -c "$source" -o "$object"
 objects+=("$object")
done
"${CXX:-c++}" -std=c++17 -O2 -pthread -I. -ICpp/Source task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
[[ "$mode" == build ]] && exit 0
if [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -O2 -pthread -I. -ICpp/Source -x c++ task_support/driver.c tests/subsurface_transport.cpp -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in density relocation transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
echo BASELINE_OK
