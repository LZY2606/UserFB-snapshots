#!/bin/bash
set -euo pipefail
mkdir -p .task-build/include .task-build/no-plugins
cat > .task-build/include/mlt_export.h <<'HEADER'
#ifndef TASK_MLT_EXPORT_H
#define TASK_MLT_EXPORT_H
#define MLT_EXPORT
#define MLT_NO_EXPORT
#define MLT_DEPRECATED __attribute__((deprecated))
#define MLT_DEPRECATED_EXPORT MLT_EXPORT MLT_DEPRECATED
#define MLT_DEPRECATED_NO_EXPORT MLT_NO_EXPORT MLT_DEPRECATED
#endif
HEADER
objects=()
for source in src/framework/*.c; do
 object=.task-build/$(basename "$source" .c).o
 objects+=("$object")
 if [[ ! -e $object || $source -nt $object ]] || find src/framework -name '*.h' -newer "$object" -print -quit 2>/dev/null | grep -q .; then
  cc -std=c11 -D_GNU_SOURCE -O1 -Isrc -Isrc/framework -I.task-build/include -I. \
   '-DPREFIX_LIB=".task-build/no-plugins"' '-DPREFIX_DATA="task_support"' \
   -pthread -c "$source" -o "$object"
 fi
done
rm -f .task-build/libmlt.a
ar rcs .task-build/libmlt.a "${objects[@]}"
