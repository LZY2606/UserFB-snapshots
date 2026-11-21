#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/prod-*.o
cc -std=c99 -O2 -I. -DMINIMP3_CHECK_CRC -c devcheck/production.c -o .task-build/prod-0.o
cc -std=c99 -O2 -I. -c devcheck/production.c -o .task-build/legacy.o
cc -std=c99 -O2 -I. -c devcheck/reference.c -o .task-build/reference.o
index=1
while IFS= read -r source; do
 cc -std=c99 -O2 -I. -DMINIMP3_CHECK_CRC -c "$source" -o ".task-build/prod-$index.o"
 index=$((index+1))
done < <(find . -type f -name '*.c' ! -path './devcheck/*' ! -path './tests/*' ! -path './task_support/*' ! -path './.task-build/*' | LC_ALL=C sort)
cc -std=c99 -O2 -I. devcheck/baseline.c .task-build/legacy.o -lm -o .task-build/baseline
