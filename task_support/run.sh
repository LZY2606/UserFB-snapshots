#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/relocation.cpp; fi
mkdir -p .task-build
prod="src/OpenMesh/Core/Mesh/ArrayKernel.cc src/OpenMesh/Core/Mesh/PolyConnectivity.cc src/OpenMesh/Core/Mesh/TriConnectivity.cc src/OpenMesh/Core/Utils/BaseProperty.cc src/OpenMesh/Core/System/omstream.cc"
for src in $prod; do
  obj=.task-build/$(basename "$src").o
  ${CXX:-c++} -std=c++14 -O1 -w -Isrc -DOM_INCLUDE_TEMPLATES -c "$src" -o "$obj"
done
${CXX:-c++} -std=c++14 -O1 -w -Isrc -DOM_INCLUDE_TEMPLATES task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++14 -O1 -w -iquote . -Isrc -DOM_INCLUDE_TEMPLATES tests/relocation.cpp task_support/driver.c .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in placement geometry sequence compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
