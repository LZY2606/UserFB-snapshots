#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
build=.task-build
if [[ "$mode" == target ]]; then
  test -s test/nes_nonlinear_target.cpp
  cmake -S task_support -B "$build" -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=ON
  cmake --build "$build" --target task_target --parallel 2
  "$build/task_target" > "$build/target.stdout" 2>&1 || { rc=$?; cat "$build/target.stdout"; exit "$rc"; }
  cat "$build/target.stdout"
  awk '
    /^TARGET_GROUP (dac|timeline|nsf) checks=[1-9][0-9]* PASS$/ { if (seen[$2]++) exit 1; n++ }
    /^TARGET_NES_NONLINEAR_COMPLETE groups=3$/ { done++ }
    END { if(n!=3 || done!=1 || !seen["dac"] || !seen["timeline"] || !seen["nsf"]) exit 1 }
  ' "$build/target.stdout"
fi
cmake -S task_support -B "$build" -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=OFF
cmake --build "$build" --target task_baseline old_basics old_basics_mem compare_wav --parallel 2
"$build/task_baseline"
"$build/old_basics" test.nsf "$build/from-file.wav"
"$build/old_basics_mem" test.nsf "$build/from-memory.wav"
"$build/compare_wav" "$build/from-file.wav" "$build/from-memory.wav"
printf '%s\n' BASELINE_OK
