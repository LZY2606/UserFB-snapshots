#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/gradient_transport.cpp; fi
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
 [[ "$source" == src/MiniLight.cpp ]] && continue
 object=".task-build/$(basename "${source%.cpp}").o"
 "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -I. -Isrc -c "$source" -o "$object"
 objects+=("$object")
done
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -I. -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -I. -Isrc src/MiniLight.cpp "${objects[@]}" -o .task-build/minilight
[[ "$mode" == build ]] && exit 0
if [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -I. -Isrc -x c++ task_support/driver.c tests/gradient_transport.cpp -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coupling accumulation reconstruction integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/minilight .task-build/legacy.ml >.task-build/legacy-cli.log
grep -qx finished .task-build/legacy-cli.log
test -s .task-build/legacy.ml.ppm
echo LEGACY_CLI_OK
