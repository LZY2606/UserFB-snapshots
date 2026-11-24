#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=gnu99 -w -pthread -DDUMA_LIB_NO_CPP_SUPPORT -DDUMA_NO_GLOBAL_MALLOC_FREE -DDUMA_EXPLICIT_INIT -DDUMA_PREFER_GETENV -DDUMA_NO_HANG_MSG -I.)
${CC:-cc} "${flags[@]}" createconf.c -o .task-build/createconf
.task-build/createconf
rm -f .task-build/*.o
units=(duma print sem_inc)
if [[ -f duma_access.c ]]; then units+=(duma_access); fi
for unit in "${units[@]}"; do
  ${CC:-cc} "${flags[@]}" -c "$unit.c" -o ".task-build/$unit.o"
done
${CC:-cc} "${flags[@]}" task_support/baseline.c  .task-build/*.o -o .task-build/baseline
${CC:-cc} "${flags[@]}" task_support/old_healthy.c .task-build/*.o -o .task-build/old_healthy
