#!/bin/bash
set -euo pipefail
mkdir -p .task-build/production
cat > .task-build/config.h <<'CONFIG'
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
CONFIG
while IFS= read -r unit; do
  obj=${unit//\//_}
  cc -std=c99 -O1 -I.task-build -Isrc -I. -c "$unit" -o ".task-build/production/${obj%.c}.o"
done < <(find src -type f -name '*.c' | LC_ALL=C sort)
cc -std=c99 -O1 -I.task-build -Isrc -I. task_support/baseline.c .task-build/production/*.o -o .task-build/baseline
