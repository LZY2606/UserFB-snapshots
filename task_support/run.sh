#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/build_session.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++17 -O1 -I. kt_bvh.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s kt_bvh_session.h
test -s examples/build_session.cpp
${CXX:-c++} -std=c++17 -O1 -I. -include kt_bvh_session.h -c kt_bvh.cpp -o .task-build/production.o
${CXX:-c++} -std=c++17 -O1 -I. -x c++ task_support/driver.c tests/build_session.cpp -x none .task-build/production.o -o .task-build/target
${CXX:-c++} -std=c++17 -O1 -I. -include kt_bvh_session.h examples/build_session.cpp .task-build/production.o -o .task-build/example
example_dir=$(mktemp -d)
trap 'rm -rf "$example_dir"' EXIT
.task-build/example "$example_dir/session.ppm"
test -s "$example_dir/session.ppm"
head -n 1 "$example_dir/session.ppm" | grep -qx P3
rm -rf "$example_dir"
trap - EXIT
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in budgets ownership queries; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
