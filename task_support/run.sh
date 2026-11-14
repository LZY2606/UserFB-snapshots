#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/timed_path.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_SHARED_LIBS=OFF -DUSE_OMP=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=ON -DVIDSTAB_LPSOLVER=builtin -DCMAKE_C_COMPILER=cc -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1
cmake --build .task-build/lib --parallel 2 >.task-build/build.log 2>&1
cc -std=c99 -DVIDSTAB_STATIC_DEFINE -I. -Isrc -I.task-build/lib task_support/baseline.c .task-build/lib/libvidstab.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/timed_stabilize.c
  cc -std=c99 -DVIDSTAB_STATIC_DEFINE -I. -Isrc -I.task-build/lib task_support/driver.c tests/timed_path.c .task-build/lib/libvidstab.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timed_math timed_io timed_render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
  cc -std=c99 -DVIDSTAB_STATIC_DEFINE -I. -Isrc -I.task-build/lib examples/timed_stabilize.c .task-build/lib/libvidstab.a -lm -o .task-build/timed-demo
  .task-build/timed-demo .task-build/timed-output.gray
  [[ $(wc -c < .task-build/timed-output.gray) -eq 6144 ]]
fi
.task-build/baseline
