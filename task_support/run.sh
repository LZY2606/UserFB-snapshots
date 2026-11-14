#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s renderer/tests/ellipsoid_target.c; fi
mkdir -p .task-build
objects=()
for source in renderer/core/graphics.c renderer/core/maths.c; do
 object=.task-build/$(basename "$source" .c).o
 cc -std=c99 -O2 -I. -c "$source" -o "$object"
 objects+=("$object")
done
if [[ -f renderer/core/ellipsoid.c ]]; then
 cc -std=c99 -O2 -I. -c renderer/core/ellipsoid.c -o .task-build/ellipsoid.o
 objects+=(.task-build/ellipsoid.o)
fi
cc -std=c99 -O2 -I. task_support/baseline.c "${objects[@]}" -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. task_support/driver.c renderer/tests/ellipsoid_target.c "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in depth surface pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
