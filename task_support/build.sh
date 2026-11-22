#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c99 -O0 -g -DRELEASE -DHAVE_INTTYPES_H -DHAVE_STDBOOL_H -DHAVE_UNISTD_H -DHAVE_SYS_TIME_H -DHAVE_STRERROR_R -Iinclude -I.)
case $(uname -s) in Darwin) dead=(-Wl,-dead_strip);; *) dead=(-Wl,--gc-sections);; esac
objects=()
for source in src/rtp/*.c src/jbuf/*.c src/mem/*.c src/list/*.c src/mbuf/*.c src/fmt/*.c src/dbg/*.c src/tmr/*.c src/sa/*.c; do
  object=.task-build/$(echo "$source" | tr / _).o
  cc "${flags[@]}" -ffunction-sections -fdata-sections -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/production.a "${objects[@]}"
cc "${flags[@]}" task-baseline.c .task-build/production.a "${dead[@]}" -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  cc "${flags[@]}" task_support/driver.c checks/red_playout.c .task-build/production.a "${dead[@]}" -o .task-build/target
fi
