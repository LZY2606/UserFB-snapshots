#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build
objects=()
for file in rasterizer/*.cpp rasterizer/math/*.cpp; do
  [[ "$file" == rasterizer/texture.cpp ]] && continue
  object=".task-build/$(basename "${file%.cpp}").o"
  c++ -std=c++20 -O2 -I . -I rasterizer -c "$file" -o "$object"
  objects+=("$object")
done
if [[ $mode == target ]]; then
  c++ -std=c++20 -O2 -I . -I rasterizer "${objects[@]}" task_support/driver.c checks/target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in lookup integral material; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
sh harness/run-baseline.sh
c++ -std=c++20 -O1 -I . checks/table_probe.cpp -o .task-build/table_probe
.task-build/table_probe
c++ -std=c++20 -O2 -I . -I rasterizer checks/hdr_probe.cpp .task-build/renderer.o .task-build/utility.o -o .task-build/hdr_probe
.task-build/hdr_probe
