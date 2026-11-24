#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in Horde3D/Source/Horde3DEngine/egMotionMatching.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/motion_matching.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++14 -DH3D_STATIC_LIBS -DCMAKE -I. -IHorde3D/Source/Horde3DEngine -IHorde3D/Source/Shared -IHorde3D/Bindings/C++ -I.task-build -x c++ task_support/driver.c tests/motion_matching.cpp -x none .task-build/libHorde3D.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in features search handoff integration invalid; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/motion_baseline
