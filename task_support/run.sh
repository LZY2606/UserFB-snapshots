#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/vr_endpoint.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DWITH_OPENMP=OFF -DWITH_PFFFT=OFF -DWITH_CR32S=OFF -DWITH_CR64S=OFF -DWITH_LSR_BINDINGS=OFF -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF >.task-build/configure.log 2>&1
cmake --build .task-build/cmake --target soxr 1-delay-clear -j2 >.task-build/build.log 2>&1
cc -std=c99 -I. -Isrc task_support/baseline.c .task-build/cmake/src/libsoxr.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Isrc tests/vr_endpoint.c task_support/driver.c .task-build/cmake/src/libsoxr.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in length slew callback; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/cmake/tests/1-delay-clear >.task-build/old-delay.log 2>&1
echo OLD_DELAY_CLEAR_OK
