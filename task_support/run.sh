#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/vsm_target.c; fi
mkdir -p .task-build
if [[ -s vsm.c ]]; then
  cc -std=c99 -O2 -I. task_support/baseline.c vsm.c -lm -o .task-build/baseline
else
  cc -std=c99 -O2 -I. task_support/baseline.c -lm -o .task-build/baseline
fi
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c mini3d.c -o .task-build/mini3d.o
if [[ $mode == target ]]; then
  test -s vsm.c
cc -std=c99 -O2 -I. -DMINI3D_HEADLESS task_support/driver.c tests/vsm_target.c .task-build/mini3d.o vsm.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in moment_capture moment_filter camera_shading; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
