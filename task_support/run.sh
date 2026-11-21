#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/nurbs_silhouette.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -IGTE task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s GTE/Mathematics/NURBSSilhouette.h
test -s GTE/Mathematics/NURBSSilhouette.cpp
test -s examples/nurbs_silhouette.cpp
c++ -std=c++17 -O1 -IGTE -I. -include Mathematics/NURBSSilhouette.h GTE/Mathematics/NURBSSilhouette.cpp task_support/driver.c tests/nurbs_silhouette.cpp -o .task-build/target
c++ -std=c++17 -O1 -IGTE -I. -include Mathematics/NURBSSilhouette.h GTE/Mathematics/NURBSSilhouette.cpp examples/nurbs_silhouette.cpp -o .task-build/example
rm -f .task-build/silhouette.svg
.task-build/example .task-build/silhouette.svg
test -s .task-build/silhouette.svg
grep -q '<svg' .task-build/silhouette.svg
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology visibility failure; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
