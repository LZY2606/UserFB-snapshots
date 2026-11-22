#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/aiff_splice_target.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build -j 4
if [[ $mode == target ]]; then
  objects=()
while IFS= read -r -d '' object; do objects+=("$object"); done < <(find .task-build/CMakeFiles/audiofile_task.dir -name '*.o' -print0)
[[ ${#objects[@]} -gt 0 ]]
c++ -std=c++11 -I. -Ilibaudiofile task_support/driver.c test/aiff_splice_target.cpp "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layout anchors live_handle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
