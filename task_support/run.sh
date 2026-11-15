#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/decoder/DecUT_422_task.cpp; fi
mkdir -p .task-build
make -j2 libopenh264.a BUILDTYPE=Release USE_ASM=No ENABLEPIC=No > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O2 -I. -Icodec/api/wels -Itask_support -x c++ task_support/driver.c test/decoder/DecUT_422_task.cpp -x none libopenh264.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in chroma pipeline compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 ${CXX:-c++} -std=c++11 -O2 -Icodec/api/wels task_support/baseline.cpp libopenh264.a -pthread -o .task-build/baseline
 .task-build/baseline
fi
