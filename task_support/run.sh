#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/corpus_model.cpp; fi
bash checks/build.sh
"${CXX:-clang++}" -std=c++11 -O1 -g -fsanitize-coverage=inline-8bit-counters,pc-table checks/merge_fixture.cpp .task-build/libFuzzer.a -pthread -o .task-build/merge-fixture
if [[ $mode == target ]]; then
  "${CXX:-clang++}" -std=c++11 -O1 -I. -Ilib/fuzzer -x c++ task_support/driver.c checks/corpus_model.cpp checks/link_callback.cpp -x none .task-build/libFuzzer.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in journal projection ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"${CXX:-clang++}" -std=c++11 -O1 -Ilib/fuzzer checks/upstream_baseline.cpp .task-build/libFuzzer.a -pthread -o .task-build/baseline
.task-build/baseline
bash checks/finite_merge.sh
