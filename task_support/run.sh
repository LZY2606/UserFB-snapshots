#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/cue_split.c; fi
cmake -S . -B .task-build/flac -DBUILD_SHARED_LIBS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DWITH_OGG=OFF -DWITH_ASM=OFF -DENABLE_MULTITHREADING=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Iconv=ON
cmake --build .task-build/flac --target FLAC -j 4
cc -std=c11 -DFLAC__NO_DLL -Iinclude task_support/baseline.c .task-build/flac/src/libFLAC/libFLAC.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -DFLAC__NO_DLL -I. -Iinclude task_support/driver.c tests/cue_split.c .task-build/flac/src/libFLAC/libFLAC.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in routing metadata streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
