#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/pcm_framewrap.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  objects=(.task-build/cmake/mxf/libMXF.a .task-build/libtaskuuid.a); extra=(); for source in examples/pcmwrap/*.c; do [[ -f "$source" ]] || continue; [[ $source == examples/pcmwrap/main.c ]] && continue; extra+=(".task-build/pcmwrap-$(basename "$source" .c).o"); done; cc -D__FILENAME__=__FILE__ -std=c99 -O2 -I. -Itask_support -I.task-build/cmake test/pcm_framewrap.c task_support/driver.c ${extra[@]+"${extra[@]}"} "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cadence layout failure; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
