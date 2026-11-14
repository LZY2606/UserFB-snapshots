#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/karaoke_test.c; fi
mkdir -p .task-build
caption_sources=()
for source_file in src/*.c; do
 case "$source_file" in src/dvtcc.c|src/eia608_from_utf8.c) continue ;; esac
 caption_sources+=("$source_file")
done
cc -std=c99 -Icaption -I. "${caption_sources[@]}" -x c src/eia608_from_utf8.c.cached -x none task_support/baseline.c -lm -o .task-build/baseline > .task-build-build.log 2>&1
if [[ $mode == target ]]; then
  cc -std=c99 -Icaption -I. "${caption_sources[@]}" -x c src/eia608_from_utf8.c.cached -x none task_support/driver.c task_support/karaoke_test.c -lm -o .task-build/target >> .task-build-build.log 2>&1
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in parse_edit phases retime_export; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
