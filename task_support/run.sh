#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in core/phase_lock.h core/phase_lock.cpp; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/phase_lock_contract.cpp; fi
bash task_support/build.sh
export ALSOFT_CONF="$PWD/task_support/alsoft.conf"
if [[ $mode == target ]]; then
  bash task_support/link.sh task_support/driver.c tests/phase_lock_contract.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in spectral tracking render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/link.sh task_support/baseline.cpp -o .task-build/baseline
.task-build/baseline
bash task_support/link.sh task_support/pitch_baseline.cpp -o .task-build/pitch-baseline
.task-build/pitch-baseline
