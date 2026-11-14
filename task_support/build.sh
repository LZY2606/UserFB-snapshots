#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
while IFS= read -r source_file; do
 object_file=".task-build/$(printf '%s' "$source_file" | tr / _).o"
 cc -std=c99 -O0 -D_POSIX_C_SOURCE=200809L -DMyCORE_BUILD_WITHOUT_THREADS -Isource -I. -c "$source_file" -o "$object_file"
 objects+=("$object_file")
done < <(find source/mycore source/myencoding source/myhtml source/myport/posix -name '*.c' -type f | LC_ALL=C sort)
ar rcs .task-build/libmyhtml.a "${objects[@]}"
cc -std=c99 -O0 -DMyCORE_BUILD_WITHOUT_THREADS -Isource -I. task_support/baseline.c .task-build/libmyhtml.a -o .task-build/baseline
