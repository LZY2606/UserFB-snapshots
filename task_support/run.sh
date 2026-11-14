#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in mini3d.h motion.h motion.c; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/motion_target.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. support/baseline.c -lm -o .task-build/baseline
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c mini3d.c -o .task-build/core.o
if [[ $mode == target ]]; then
  if [[ -f motion.c ]]; then
  cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c motion.c -o .task-build/motion.o
else
  cc -x c -c /dev/null -o .task-build/motion.o
fi
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. task_support/driver.c tests/motion_target.c .task-build/core.o .task-build/motion.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in events integrals pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
