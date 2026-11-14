#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c99 -O0 -Itask_support -Isrc -Isrc/common)
for unit in fd fuzz random ranges; do
  ${CC:-cc} "${flags[@]}" -c "src/common/$unit.c" -o ".task-build/$unit.o"
done
${CC:-cc} "${flags[@]}" task_support/baseline.c .task-build/fd.o .task-build/fuzz.o .task-build/random.o .task-build/ranges.o -lm -o .task-build/baseline
# Compile the real command-line stdin path. It is used only as a finite byte filter.
${CC:-cc} -std=gnu99 -w -D_DARWIN_C_SOURCE -include libgen.h "${flags[@]}" -DLIBDIR='"/unused"' src/zzuf.c src/opts.c src/timer.c src/myfork.c src/util/getopt.c src/util/md5.c src/util/hex.c .task-build/fd.o .task-build/fuzz.o .task-build/random.o .task-build/ranges.o -lm -o .task-build/zzuf-stdin
