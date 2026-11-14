#!/bin/bash
set -euo pipefail
rm -rf .task-build/obj
rm -f .task-build/libcray.a
mkdir -p .task-build/obj
find src/lib src/common -name '*.c' -type f | LC_ALL=C sort > .task-build/sources.txt
while IFS= read -r source_path; do
  object_path=".task-build/obj/${source_path%.c}.o"
  mkdir -p "$(dirname "$object_path")"
  cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -I. -Iinclude -Isrc -Isrc/lib -Isrc/common -c "$source_path" -o "$object_path"
done < .task-build/sources.txt
sed 's/@GIT_SHA1@/1fce01f4f8f06e45f1b82f339eb9ab29ee1258dd/g' generated/gitsha1.c.in > .task-build/gitsha1.c
cc -std=c99 -c .task-build/gitsha1.c -o .task-build/obj/gitsha1.o
find .task-build/obj -name '*.o' -type f | LC_ALL=C sort > .task-build/objects.txt
ar rcs .task-build/libcray.a $(cat .task-build/objects.txt)
