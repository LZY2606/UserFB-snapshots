#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/encoder/EncUT_Pcm_task.cpp; fi
mkdir -p .task-build
make -j 4 USE_ASM=No BUILDTYPE=Release libopenh264.a >.task-build/compile.log 2>&1
c++ -std=c++11 -I. -Icodec/api task_support/baseline.cpp libopenh264.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -Icodec/api -Icodec/common/inc -Icodec/encoder/core/inc -Icodec/encoder/plus/inc -Icodec/decoder/core/inc -Icodec/decoder/plus/inc -x c++ task_support/driver.c test/encoder/EncUT_Pcm_task*.cpp -x none libopenh264.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in syntax reference ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
