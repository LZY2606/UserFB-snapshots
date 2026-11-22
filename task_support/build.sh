#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
includes=(-Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include)
objects=()
for file in lib/*.c task_support/vendor/ogg/src/bitwise.c task_support/vendor/ogg/src/framing.c; do
 case "$file" in lib/psytune.c|lib/tone.c|lib/barkmel.c) continue;; esac
 object=.task-build/obj/$(basename "${file%.c}").o
 if [[ ! -f $object || $file -nt $object ]] || find include lib task_support/include task_support/vendor/ogg/include -name '*.h' -newer "$object" -print -quit 2>/dev/null | grep -q .; then
  cc -std=c99 -O1 -DHAVE_ALLOCA_H -DVORBIS_PENDING_TRACE "${includes[@]}" -c "$file" -o "$object"
 fi
 objects+=("$object")
done
rm -f .task-build/libcodec.a
ar rcs .task-build/libcodec.a "${objects[@]}"
cc -std=c99 -O1 "${includes[@]}" task_support/baseline.c .task-build/libcodec.a -lm -o .task-build/baseline
