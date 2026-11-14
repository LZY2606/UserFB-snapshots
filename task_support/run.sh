#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/vp9_spatial.cpp; fi
mkdir -p .task-build
CPP_FLAGS=(-std=c++17 -O1 -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0 -DRTC_STATIC -I. -Isrc -Iinclude/rtc -Ideps/plog/include)
objects=()
while IFS= read -r path; do
  object=".task-build/$(echo "$path" | tr '/' '_').o"
  c++ "${CPP_FLAGS[@]}" -c "$path" -o "$object"
  objects+=("$object")
done < <(find src -type f -name '*.cpp' | sort)
[[ ${#objects[@]} -gt 0 ]]
c++ "${CPP_FLAGS[@]}" task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ "${CPP_FLAGS[@]}" task_support/driver.c task_tests/vp9_spatial.cpp "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in early layers continuity; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
