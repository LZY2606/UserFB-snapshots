#!/bin/bash
set -euo pipefail
mkdir -p .task-build/production
while IFS= read -r unit; do
  obj=${unit//\//_}
  cc -std=c99 -O1 -Iinclude -I. -c "$unit" -o ".task-build/production/${obj%.c}.o"
done < <(find src -type f -name '*.c' | LC_ALL=C sort)
cc -std=c99 -O1 -Iinclude -I. task_support/baseline.c .task-build/production/*.o -lm -o .task-build/baseline
cc -std=c99 -O1 -Iinclude -I. test/indexing.c .task-build/production/*.o -lm -o .task-build/indexing
