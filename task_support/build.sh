#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
objects=()
for file in libmp3lame/*.c mpglib/*.c; do
  object=.task-build/obj/$(echo "${file%.c}" | tr / _).o
  if [[ ! -f $object || $file -nt $object ]] || find include libmp3lame mpglib task_support/include -name '*.h' -newer "$object" -print -quit | grep -q .; then
    cc -std=c99 -O1 -DHAVE_CONFIG_H -I. -Iinclude -Ilibmp3lame -Impglib -Itask_support/include -c "$file" -o "$object"
  fi
  objects+=("$object")
done
# Recreate the archive so removed production files cannot remain linked.
if [[ -e .task-build/libmp3lame.a ]]; then unlink .task-build/libmp3lame.a; fi
ar rcs .task-build/libmp3lame.a "${objects[@]}"
cc -std=c99 -O1 -Iinclude task_support/baseline.c .task-build/libmp3lame.a -lm -o .task-build/baseline
