#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2 ;; esac
if [[ $mode == target ]]; then test -s tests/debug_origin_cases.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/library -DBUILD_SHARED_LIBS=OFF -DBUILD_COMMAND_LINE_TOOLS=OFF -DPEPARSE_ENABLE_TESTING=OFF -DPEPARSE_ENABLE_EXAMPLES=OFF -DCMAKE_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build/library --target pe-parse -j2
if [[ $mode == target ]]; then
  test -s examples/debug_origin/main.cpp
test -s pe-parser-library/include/pe-parse/debug_records.h
test -s docs/debug-origin-analysis.md
"${CXX:-c++}" -std=c++17 -I. -Ipe-parser-library/include task_support/driver.c tests/debug_origin_cases.cpp .task-build/library/pe-parser-library/libpe-parse.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in origins identities integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  "${CXX:-c++}" -std=c++17 -Ipe-parser-library/include task_support/baseline.cpp .task-build/library/pe-parser-library/libpe-parse.a -o .task-build/baseline
  .task-build/baseline
  "${CXX:-c++}" -std=c++17 -Ipe-parser-library/include task_support/sample_baseline.cpp .task-build/library/pe-parser-library/libpe-parse.a -o .task-build/sample-baseline
  .task-build/sample-baseline
elif [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -Ipe-parser-library/include examples/debug_origin/main.cpp .task-build/library/pe-parser-library/libpe-parse.a -o .task-build/debug-origin
  "${CXX:-c++}" -std=c++17 task_support/report_check.cpp -o .task-build/report-check
  .task-build/debug-origin samples/cedar32.pe samples/cedar64.pe > .task-build/debug-origins.tsv
  .task-build/report-check .task-build/debug-origins.tsv samples/cedar32.pe samples/cedar64.pe
  echo PE_DEBUG_ORIGIN_TARGET_COMPLETE
else
  exit 2
fi
