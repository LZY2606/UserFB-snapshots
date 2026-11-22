#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/frontend_contract.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -fwrapv -I. -Iamrwbenc/inc -Icommon/include task_baseline.c wrapper.c common/cmnMemory.c amrwbenc/src/*.c -o .task-build/baseline > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -fwrapv -DTASK_FRONTEND_TEST -I. -Iamrwbenc/inc -Icommon/include task_support/driver.c tests/*.c wrapper.c common/cmnMemory.c amrwbenc/src/*.c -o .task-build/target > .task-build/target-build.log 2>&1 || { tail -30 .task-build/target-build.log; exit 1; }
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Polyphase Preview Scaling Streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
