#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for file in common/cmnMemory.c aacenc/basic_op/*.c aacenc/src/*.c; do
  obj=.task-build/$(basename "${file%.c}").o
  cc -std=c99 -O2 -fwrapv -D__unused= -I. -Iaacenc/inc -Iaacenc/basic_op -Icommon/include -c "$file" -o "$obj"
  objects+=("$obj")
done
ar rcs .task-build/libcodec.a "${objects[@]}"
cc -std=c99 -O2 -fwrapv -D__unused= -I. -Iaacenc/inc -Iaacenc/basic_op -Icommon/include task_support/baseline.c .task-build/libcodec.a -lm -o .task-build/baseline
