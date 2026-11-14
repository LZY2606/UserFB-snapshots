#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in demo/delta_mush.h demo/delta_mush.cpp; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/delta_mush.cpp; fi
mkdir -p .task-build
extra_object=""
if [[ -f demo/delta_mush.cpp ]]; then
  c++ -std=c++17 -O2 -DIQM_HEADLESS -I. -c demo/delta_mush.cpp -o .task-build/delta_mush.o
  extra_object=.task-build/delta_mush.o
fi
c++ -std=c++17 -O2 -DIQM_HEADLESS -I. -c demo/demo.cpp -o .task-build/demo.o
c++ -std=c++17 -O2 -DIQM_HEADLESS -I. task_support/current.cpp .task-build/demo.o $extra_object -o .task-build/current
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -DIQM_HEADLESS -I. -x c++ task_support/driver.c tests/delta_mush.cpp -x none .task-build/demo.o $extra_object -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in diffusion fit pipeline ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/current
