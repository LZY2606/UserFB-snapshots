#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c99 -D_DEFAULT_SOURCE -DNO_CRYPTO -DRTMPDUMP_VERSION=\"v2.4\" -O0 -g -I. -Ilibrtmp)
objects=()
for source in librtmp/*.c; do
    object=.task-build/$(basename "$source" .c).o
    cc "${flags[@]}" -Dsend=task_memory_send -Drecv=task_memory_recv -c "$source" -o "$object"
    objects+=("$object")
done
ar rcs .task-build/production.a "${objects[@]}"
cc "${flags[@]}" task-baseline.c task_support/memory_io.c .task-build/production.a -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
    cc "${flags[@]}" task_support/driver.c checks/livepacer.c task_support/memory_io.c .task-build/production.a -lm -o .task-build/target
fi
