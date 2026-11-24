#!/bin/bash
set -euo pipefail
test -s test/alloc_fault.c
mkdir -p .task-build/fault
rm -f .task-build/fault/*.o .task-build/fault/libtheft.a
for source_file in src/*.c; do
  cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -Dmalloc=qa_malloc -Dcalloc=qa_calloc -Drealloc=qa_realloc -Dfree=qa_free -Iinc -Isrc -Ivendor -I.task-build -c "$source_file" -o ".task-build/fault/$(basename "${source_file%.c}").o"
done
ar -rcs .task-build/fault/libtheft.a .task-build/fault/*.o
