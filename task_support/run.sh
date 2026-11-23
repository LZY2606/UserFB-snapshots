#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/periodic.cpp; fi
mkdir -p .task-build
rm -f .task-build/prod-*.o
objects=()
for src in include/pandaresampler/pandaresampler.cc task_src/*.cpp task_src/*.cc; do
  [[ -f "$src" ]] || continue
  obj=".task-build/prod-${#objects[@]}.o"
  c++ -std=c++17 -O2 -I. -Iinclude -Itask_src -c "$src" -o "$obj"
  objects+=("$obj")
done
c++ -std=c++17 -O2 -I. -Iinclude probes/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Iinclude -Itask_src -x c++ task_support/driver.c task_tests/periodic.cpp -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in closure orbit streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
