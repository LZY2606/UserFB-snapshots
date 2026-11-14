#!/bin/bash
set -euo pipefail
mkdir -p .task-build/include/wabt .task-build/obj
if ! cmp -s task_support/config.h .task-build/include/wabt/config.h; then
  cp task_support/config.h .task-build/include/wabt/config.h
fi
# Source enumeration includes future production .cc/.c files in src/.
find src -name '*.cc' -o -name '*.c' | LC_ALL=C sort > .task-build/sources.txt
objects=()
while IFS= read -r source; do
  [[ "$source" == src/prebuilt/* ]] && continue
  object=".task-build/obj/${source//\//_}.o"
  if [[ ! -f "$object" || "$source" -nt "$object" ]] || find include task_support/config.h -type f -newer "$object" -print -quit 2>/dev/null | read -r unused; then
    if [[ "$source" == *.c ]]; then
      ${CC:-cc} -std=c11 -O0 -I. -Iinclude -I.task-build/include -c "$source" -o "$object"
    else
      ${CXX:-c++} -std=c++20 -O0 -I. -Iinclude -I.task-build/include -c "$source" -o "$object"
    fi
  fi
  objects+=("$object")
done < .task-build/sources.txt
ar rcs .task-build/libwabt.a "${objects[@]}"
${CXX:-c++} -std=c++20 -O0 -I. -Iinclude -I.task-build/include task_support/baseline.cc .task-build/libwabt.a -o .task-build/baseline
