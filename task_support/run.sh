#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/closed_loop.c; fi
mkdir -p .task-build
flags=(-std=c99 -O2 -Wno-deprecated-non-prototype -Wno-comment -I. -Iinc -DSASR -DWAV49 -DNeedFunctionPrototypes=1)
sources=(src/add.c src/code.c src/debug.c src/decode.c src/long_term.c src/lpc.c src/preprocess.c src/rpe.c src/gsm_destroy.c src/gsm_decode.c src/gsm_encode.c src/gsm_explode.c src/gsm_implode.c src/gsm_create.c src/gsm_print.c src/gsm_option.c src/short_term.c src/table.c)
objects=()
for src in "${sources[@]}"; do
 obj=".task-build/$(basename "${src%.c}").o"
 cc "${flags[@]}" -c "$src" -o "$obj"
 objects+=("$obj")
done
ar rcs .task-build/library.a "${objects[@]}"
if [[ $mode == target ]]; then
  cc "${flags[@]}" task_support/driver.c tests/closed_loop.c .task-build/library.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ltp rpe loop; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc "${flags[@]}" task_support/baseline.c .task-build/library.a -lm -o .task-build/baseline
.task-build/baseline
