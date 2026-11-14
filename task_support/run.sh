#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_strip.cpp; fi
mkdir -p .task-build
for name in wave propagator wfc; do
  c++ -std=c++17 -O2 -Isrc/include -c src/lib/$name.cpp -o .task-build/$name.o
done
c++ -std=c++17 -O2 -Isrc/include task_support/baseline.cpp .task-build/wave.o .task-build/propagator.o .task-build/wfc.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Isrc/include -x c++ task_support/driver.c tests/task_strip.cpp -x none .task-build/wave.o .task-build/propagator.o .task-build/wfc.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in probability sampling image; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
