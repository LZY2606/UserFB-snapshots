#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;; esac
# Each build uses only local source, the system compiler, and CMake.
export CCACHE_DISABLE=1
export HTTP_PROXY=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 ALL_PROXY=http://127.0.0.1:9
export http_proxy="$HTTP_PROXY" https_proxy="$HTTPS_PROXY" all_proxy="$ALL_PROXY"
unset NO_PROXY no_proxy
export CMAKE_BUILD_PARALLEL_LEVEL=4
if [[ $mode == target ]]; then test -s test/supplementary_macro_contract.c; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Debug -DENABLE_DECOMPRESSION=OFF -DDO_TESTING=ON -DBUILD_DWARFGEN=OFF -DBUILD_DWARFEXAMPLE=OFF
cmake --build .task-build --target dwarf dwarfdump selfmacrocheck -j 4
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Itask_support -Isrc/lib/libdwarf -I.task-build -I.task-build/src/lib/libdwarf task_support/driver.c task_support/fixture.c test/supplementary_macro_contract.c .task-build/src/lib/libdwarf/libdwarf.a -o .task-build/target
export TASK_DWARFDUMP="$PWD/.task-build/src/bin/dwarfdump/dwarfdump"
export TASK_BUILD="$PWD/.task-build"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in owner_context import_execution recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -I. -Itask_support -Isrc/lib/libdwarf -I.task-build -I.task-build/src/lib/libdwarf task_support/baseline.c task_support/fixture.c .task-build/src/lib/libdwarf/libdwarf.a -o .task-build/baseline
.task-build/baseline .task-build
.task-build/test/selfmacrocheck
if [[ $mode == baseline ]]; then
.task-build/src/bin/dwarfdump/dwarfdump --file-name=src/bin/dwarfdump/dwarfdump.conf --print-macinfo --file-tied=.task-build/sup.elf .task-build/main.elf
fi
