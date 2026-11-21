#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/wavl_ima_contract.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude task_support/driver.c tests/wavl_ima_contract.c .task-build/production/upstream/CMakeFiles/sndfile.dir/src/wavl.c.o .task-build/production/upstream/libsndfile.a -lm -o .task-build/target
cc -std=c99 -I. -Iinclude examples/wavl_ima.c .task-build/production/upstream/CMakeFiles/sndfile.dir/src/wavl.c.o .task-build/production/upstream/libsndfile.a -lm -o .task-build/demo
.task-build/demo >.task-build/demo.log 2>&1
cat .task-build/demo.log
grep -q '^WAVL_IMA_DEMO_OK$' .task-build/demo.log
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in decoding seeking bounds; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/production/baseline
