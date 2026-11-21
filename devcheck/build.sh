#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/prod-*.o
index=0
while IFS= read -r source; do
 cc -std=c99 -O2 -DSHINE_MS_TESTING -I. -Isrc/lib -c "$source" -o ".task-build/prod-$index.o"
 index=$((index+1))
done < <(find src/lib -type f -name '*.c' | LC_ALL=C sort)
cc -std=c99 -O2 -I. -c devcheck/decoder.c -o .task-build/decoder.o
cc -std=c99 -O2 -I. -Isrc/lib devcheck/baseline.c .task-build/prod-*.o .task-build/decoder.o -lm -o .task-build/baseline
