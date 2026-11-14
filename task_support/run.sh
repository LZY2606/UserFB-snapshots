#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/te_contract.cpp; fi
mkdir -p .task-build
production_sources=()
for src in pe-parser-library/src/*.cpp; do case "$src" in *unicode_winapi.cpp|*unicode_libicu.cpp) continue;; esac; production_sources+=("$src"); done
c++ -std=c++17 -iquote . -Ipe-parser-library/include "${production_sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -iquote . -Ipe-parser-library/include "${production_sources[@]}" -x c++ task_support/driver.c tests/te_contract.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coordinates directories lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline --before; else .task-build/baseline; fi
