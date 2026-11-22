#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/peak_window_test.c; fi
cmake -S . -B .task-build/production -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF -DWITH_LSR_BINDINGS=OFF -DWITH_CR32S=OFF -DWITH_CR64S=OFF -DWITH_PFFFT=OFF -DWITH_AVFFT=OFF
cmake --build .task-build/production --target soxr -j 2
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Isrc tests/peak_window_test.c task_support/driver.c .task-build/production/src/libsoxr.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interpolation streaming lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  cc -std=c99 -O2 -Isrc task_support/baseline.c .task-build/production/src/libsoxr.a -lm -o .task-build/baseline
  .task-build/baseline
fi
