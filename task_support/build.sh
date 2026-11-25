#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
objects=()
for source in libmpeg2/*.c libmpeg2/convert/*.c; do
  object=".task-build/${source//\//_}.o"
  "${CC:-cc}" -std=c99 -O2 -I task_support -I include -I libmpeg2 -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libmpeg2.a
ar rcs .task-build/libmpeg2.a "${objects[@]}"
