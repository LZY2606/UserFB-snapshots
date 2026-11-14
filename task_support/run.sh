#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/temporal_cases.c; fi
mkdir -p .task-build
source refresh_support/build.sh
cc -std=c99 -O2 -Wno-pointer-to-int-cast -D_POSIX_C_SOURCE=200112L -DH264E_MAX_THREADS=4 -I. -Irefresh_support -Irefresh_support/vendor/h264bsd -c refresh_support/encoder.c -o .task-build/encoder.o
for src in refresh_support/vendor/h264bsd/*.c; do cc -std=c99 -O2 -Wno-pointer-to-int-cast -D_POSIX_C_SOURCE=200112L -Irefresh_support -Irefresh_support/vendor/h264bsd -c "$src" -o ".task-build/$(basename "$src" .c).o"; done
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DH264E_MAX_THREADS=4 -I. -Irefresh_support -Irefresh_support/vendor/h264bsd task_support/driver.c tests/temporal_cases.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in schedule budget extraction compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash refresh_support/baseline.sh && bash refresh_support/old_test.sh
