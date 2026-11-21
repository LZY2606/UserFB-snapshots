#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/edit_values.cpp; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libtimedtext.a
for source_path in src/*.cpp; do
  object_path=".task-build/$(basename "${source_path%.cpp}").o"
  c++ -std=c++11 -O0 -w -Iinclude -Isrc -I. -c "$source_path" -o "$object_path"
done
ar rcs .task-build/libtimedtext.a .task-build/*.o
if [[ $mode == target ]]; then
  c++ -std=c++11 -O0 -w -Iinclude -I. task_support/driver.c tests/edit_values.cpp .task-build/libtimedtext.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ownership preview visitor; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -O0 -w -Iinclude -I. task_support/baseline.cpp .task-build/libtimedtext.a -o .task-build/baseline
.task-build/baseline
