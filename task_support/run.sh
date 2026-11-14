#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/volume_transport.cpp; fi
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
 [[ -f "$source" ]] || continue
 object=".task-build/$(basename "${source%.cpp}").o"
 "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -pthread -include mutex -I. -Itask_support/serial -c "$source" -o "$object"
 objects+=("$object")
done
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -pthread -include mutex -I. -Itask_support/serial minpt.cpp ${objects[@]+"${objects[@]}"} -o .task-build/minpt
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -pthread -include mutex -I. -Itask_support/serial task_support/baseline.cpp ${objects[@]+"${objects[@]}"} -o .task-build/baseline
[[ "$mode" == build ]] && exit 0
if [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -pthread -include mutex -I. -Itask_support/serial -Dmain=minpt_cli_main -c minpt.cpp -o .task-build/minpt_core.o
"${CXX:-c++}" -std=c++17 -O2 -fno-fast-math -pthread -include mutex -I. -Itask_support/serial -x c++ task_support/driver.c tests/volume_transport.cpp -x none .task-build/minpt_core.o ${objects[@]+"${objects[@]}"} -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in segments phase transport mis; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
for name in a b; do
 .task-build/minpt task_support/scene.obj "" "" ".task-build/$name.pfm" 16 5 0 4 3 0 0 2 0 0 0 40 0 0 0 >".task-build/cli-$name.log"
done
.task-build/baseline
