#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/qslim_uv.cpp; fi
mkdir -p .task-build
cxx=${CXX:-c++}
flags=(-std=c++17 -O0 -DEIGEN_MPL2_ONLY -I. -Iinclude -I.deps/eigen -pthread)
needs_build() {
  [[ ! -x "$1" ]] || [[ -n $(find include .deps task_support tests -type f -newer "$1" -print -quit) ]]
}
if needs_build .task-build/baseline; then
  "$cxx" "${flags[@]}" task_support/baseline.cpp -o .task-build/baseline
fi
if needs_build .task-build/upstream-qslim; then
  "$cxx" "${flags[@]}" -Itests -I.deps/catch2 -DLIBIGL_DATA_DIR='"unused-by-generated-cylinder"' tests/main.cpp tests/include/igl/qslim.cpp -o .task-build/upstream-qslim
fi
if [[ $mode == target ]]; then
  "$cxx" "${flags[@]}" -x c++ task_support/driver.c task_tests/qslim_uv.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in objective seams sequence compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
 .task-build/baseline
(cd .task-build && ./upstream-qslim 'qslim: cylinder')
echo LIBIGL_UPSTREAM_QSLIM_CYLINDER_OK
