#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build
objects=()
for file in src/*.cpp; do
  [[ "$file" == src/Main.cpp ]] && continue
  object=".task-build/$(basename "${file%.cpp}").o"
  if [[ ! -e "$object" || "$file" -nt "$object" ]] || find src -name '*.h' -newer "$object" -print -quit 2>/dev/null | grep -q .; then
    c++ -std=c++11 -O1 -I src -c "$file" -o "$object"
  fi
  objects+=("$object")
done
if [[ $mode == target ]]; then
  c++ -std=c++11 -I . -I src "${objects[@]}" task_support/driver.c checks/target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in flatten marshal compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -I src "${objects[@]}" checks/baseline.cpp -o .task-build/baseline
.task-build/baseline
c++ -std=c++11 -I src "${objects[@]}" checks/input_probe.cpp -o .task-build/input_probe
.task-build/input_probe
c++ -std=c++11 -I src "${objects[@]}" checks/msl_baseline.cpp -o .task-build/msl_baseline
.task-build/msl_baseline
