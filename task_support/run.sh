#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/radiosity_target.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. support/baseline.c -lm -o .task-build/baseline
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c mini3d.c -o .task-build/core.o
if [[ $mode == target ]]; then
  objects=(.task-build/core.o)
if [[ -f radiosity.c ]]; then
  cc -std=c99 -O2 -I. -c radiosity.c -o .task-build/radiosity.o
  objects+=(.task-build/radiosity.o)
fi
cc -std=c99 -O2 -I. task_support/driver.c tests/radiosity_target.c "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/task-radiosity.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in visibility transport integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
