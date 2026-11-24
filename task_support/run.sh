#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/compact_phase.cc; fi
mkdir -p .task-build
c++ -std=c++11 -O2 -I. -Isource -pthread -c source/resampler.cc -o .task-build/resampler.o
c++ -std=c++11 -O2 -I. -Isource -pthread -c source/vresampler.cc -o .task-build/vresampler.o
c++ -std=c++11 -O2 -I. -Isource -pthread -c source/resampler-table.cc -o .task-build/table.o
c++ -std=c++11 -O2 -I. -Isource -pthread task_support/baseline.cc .task-build/resampler.o .task-build/vresampler.o .task-build/table.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -Isource -pthread -x c++ task_support/driver.c tests/compact_phase.cc -x none .task-build/resampler.o .task-build/vresampler.o .task-build/table.o -o .task-build/target
  output_log=.task-build/target.log
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock signal sharing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
