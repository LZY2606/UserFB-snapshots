#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/objects
objects=()
for source_file in renderer/core/*.c renderer/shaders/*.c renderer/scenes/*.c renderer/tests/test_helper.c; do
 object_file=".task-build/objects/$(basename "$source_file" .c).o"
 "${CC:-cc}" -std=c99 -O1 -ffunction-sections -fdata-sections -I. -c "$source_file" -o "$object_file"
 objects+=("$object_file")
done
rm -f .task-build/renderer.a
ar rcs .task-build/renderer.a "${objects[@]}"
bash checks/link.sh .task-build/baseline checks/baseline.c
