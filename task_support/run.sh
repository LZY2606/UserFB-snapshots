#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/x87_shape_cases.c; fi
mkdir -p .task-build
objects=()
for source in bddisasm/*.c; do
  object=".task-build/$(basename "${source%.c}").o"
  cc -std=c99 -O0 -DBDDISASM_HAS_MEMSET -DBDDISASM_HAS_VSNPRINTF -Iinc -Ibddisasm/include -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/libbddisasm.a "${objects[@]}"
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinc task_support/driver.c tests/x87_shape_cases.c .task-build/libbddisasm.a -o .task-build/target
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  set +e
  .task-build/target >"$log" 2>&1
  status=$?
  set -e
  cat "$log"
  [[ $status == 0 ]] || exit "$status"
  for group in transfers joins faults; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$log"
fi
cc -std=c99 -Iinc task_support/witness.c .task-build/libbddisasm.a -o .task-build/witness
.task-build/witness
