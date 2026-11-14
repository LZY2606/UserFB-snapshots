#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/lighttree_test.c; fi
cc=${CC:-cc}
mkdir -p .task-build
includes=(-I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON)
flags=(-std=c11 -O2 -DUNBOUND_OBJECTS)
objects=()
for src in src/core/*.c src/raytracer/*.c lib/SimplexNoise/SimplexNoise.c task_support/headless_image.c; do
  case "$src" in src/raytracer/main.c|src/raytracer/scene.c|src/raytracer/image.c) continue;; esac
  obj=".task-build/$(basename "${src%.c}").o"
  "$cc" "${flags[@]}" "${includes[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done
rm -f .task-build/libscene.a
ar rcs .task-build/libscene.a "${objects[@]}"
"$cc" "${flags[@]}" "${includes[@]}" task_support/baseline.c .task-build/libscene.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  "$cc" "${flags[@]}" "${includes[@]}" task_support/driver.c tests/lighttree_test.c .task-build/libscene.a -lm -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/lighttask.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bounds visibility transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
