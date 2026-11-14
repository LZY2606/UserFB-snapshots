#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in src/*.c; do
  object=".task-build/$(basename "${source%.c}").o"
  cc -std=c99 -O1 -D_POSIX_C_SOURCE=200809L -include strings.h -DENABLE_DSD -DENABLE_LEGACY -DENABLE_THREADS -Iinclude -Isrc -I. -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libwavpack.a
ar rcs .task-build/libwavpack.a "${objects[@]}"
cc -std=c99 -O1 -Iinclude -I. task_support/baseline.c .task-build/libwavpack.a -lm -pthread -o .task-build/baseline
