#!/usr/bin/env bash
set -euo pipefail
: "${TASK_BUILD:?}"
# Include additional library C translation units if the implementation adds them.
objs=()
for f in schrift*.c; do
 test -f "$f" || continue
 obj="$TASK_BUILD/$(basename "$f" .c).o"
 cc -std=c99 -O1 -Wall -Wextra -I. -c "$f" -o "$obj"
 objs+=("$obj")
done
((${#objs[@]} > 0))
ar rcs "$TASK_BUILD/libschrift.a" "${objs[@]}"
