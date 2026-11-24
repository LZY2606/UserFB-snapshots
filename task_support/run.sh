#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/riscv_reloc_cases.cpp; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  c++ -std=c++17 -I. -x c++ task_support/driver.c tests/riscv_reloc_cases.cpp -o .task-build/target
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  set +e
  .task-build/target >"$log" 2>&1
  status=$?
  set -e
  cat "$log"
  [[ $status == 0 ]] || exit "$status"
  for group in pairs fields objects; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$log"
fi
c++ -std=c++17 -I. examples/write_obj/write_obj.cpp -o .task-build/write_obj
(cd .task-build && ./write_obj)
c++ -std=c++17 -I. examples/tutorial/tutorial.cpp -o .task-build/tutorial
.task-build/tutorial .task-build/hello.o
c++ -std=c++17 -I. task_support/witness.cpp -o .task-build/witness
.task-build/witness
