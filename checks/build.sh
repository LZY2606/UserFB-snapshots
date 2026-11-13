#!/usr/bin/env bash
set -euo pipefail
build=${1:?build directory}
mkdir -p "$build"
objects=()
while IFS= read -r src; do
  test -s "$src"
  obj="$build/$(basename "$src" .c).o"
  "${CC:-cc}" -std=c99 -O1 -g -Iinclude ${CHECK_CFLAGS:-} -c "$src" -o "$obj"
  objects+=("$obj")
done < checks/sources.list
ar rcs "$build/libdumb.a" "${objects[@]}"
echo DUMB_BUILD_OK
