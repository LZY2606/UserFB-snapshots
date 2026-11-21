#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/repair.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -pthread -I. -include task_support/compat.h -c task_support/driver.c -o .task-build/driver.o
for src in ./*.cpp; do c++ -std=c++11 -pthread -I. -include task_support/compat.h -c "$src" -o ".task-build/$(basename "${src%.cpp}").o"; done
if [[ $mode == target ]]; then
  c++ -std=c++11 -pthread -I. -include task_support/compat.h checks/repair.cpp .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in normalize feedback playout; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -pthread -I. -include task_support/compat.h task_support/baseline.cpp $(find .task-build -maxdepth 1 -name '*.o' ! -name driver.o -print) -o .task-build/baseline
.task-build/baseline
