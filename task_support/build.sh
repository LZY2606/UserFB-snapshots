#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
objdir=.task-build/base-lib
flags=()
if [[ $mode == target ]]; then objdir=.task-build/target-lib; flags=(-DUAVS3E_TASK_TESTING=1); fi
mkdir -p "$objdir"
sources=(src/*.c)
if [[ -d task_src ]]; then
  while IFS= read -r -d '' source; do sources+=("$source"); done < <(find task_src -type f -name '*.c' -print0)
fi
objects=()
for source in "${sources[@]}"; do
  object=$objdir/${source//\//_}.o
  if [[ ! -f $object || $source -nt $object ]] || find inc -type f -newer "$object" -print -quit | grep -q .; then
    ${CC:-cc} -std=c99 -O2 -w -DUAVS3E_SCALAR_ONLY -DCOMPILE_10BIT=0 ${flags[@]+"${flags[@]}"} -Iinc -I. -c "$source" -o "$object"
  fi
  objects+=("$object")
done
rm -f "$objdir/libuavs3e.a"
ar rcs "$objdir/libuavs3e.a" "${objects[@]}"
