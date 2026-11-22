#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/metadata_edit_target.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --parallel 2
if [[ $mode == target ]]; then
  test -s examples/metadata_edit.cpp
cmake --build .task-build --target task_target metadata_edit_example --parallel 2
cp .task-build/task_target .task-build/target
.task-build/metadata_edit_example .task-build/example.wav
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in reserve_lifecycle arena_layout reopen_preserve; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 .task-build/production_witness
 (cd .task-build/test_data && ../upstream/tests/file_tests "read_rect_24bit,write_read_odd_chunks_after")
 .task-build/upstream/tests/chunk_tests "chna_chunk,axml_chunk"
fi
