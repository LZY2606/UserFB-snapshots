#!/bin/bash
set -euo pipefail
mkdir -p .task-build
printf '#define HAVE_INTTYPES_H 1\n' > .task-build/config.h
objects=()
while IFS= read -r file; do
 obj=".task-build/$(basename "${file%.c}").o"
 cc -std=c99 -O2 -I.task-build -Iinclude -Isrc -c "$file" -o "$obj"
 objects+=("$obj")
done < <(find src -name '*.c' -type f | LC_ALL=C sort)
cc -std=c99 -O2 -Iinclude task_support/baseline.c "${objects[@]}" -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 cc -std=c99 -O2 -I. -Iinclude -I.task-build -Isrc task_support/driver.c tests/sparse_stream.c "${objects[@]}" -lm -o .task-build/target
fi
