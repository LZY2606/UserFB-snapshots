#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/layer_dependency.cpp; fi
mkdir -p .task-build
shopt -s nullglob
objects=()
for source in code/ScreamTx.cpp code/ScreamV2Tx.cpp code/ScreamV2TxStream.cpp code/RtpQueue.cpp code/LayerDependency*.cpp task_support/packet_owner.cpp; do
  object=".task-build/$(basename "$source" .cpp).o"
  c++ -std=c++17 -O0 -pthread -I. -Icode -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/production.a
ar rcs .task-build/production.a "${objects[@]}"
c++ -std=c++17 -O0 -pthread -I. -Icode task_support/baseline.cpp .task-build/production.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -pthread -I. -Icode -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -pthread -I. -Icode task_tests/layer_dependency.cpp .task-build/driver.o .task-build/production.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dependency feedback playout; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
