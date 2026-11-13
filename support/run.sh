#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
mode=${1:-baseline}
PRODUCTION_SOURCES="src/clusterizer.cpp src/diskclusterizer.cpp src/allocator.cpp src/spatialorder.cpp"
compile() { ${CXX:-c++} -std=c++17 -O1 -I. -Isupport "$@" -lm; }
compile support/baseline.cpp ${PRODUCTION_SOURCES} -o "$build/baseline"
"$build/baseline"
if [ "$mode" = baseline ]; then exit 0; fi
[ "$mode" = target ] || exit 2
[ -s support/target.cpp ] || { echo 'missing support/target.cpp' >&2; exit 2; }
[ -s examples/disk_meshlets.cpp ] || { echo 'missing examples/disk_meshlets.cpp' >&2; exit 2; }
compile support/target.cpp support/driver.cpp ${PRODUCTION_SOURCES} -o "$build/target"
for group in disk boundary coverage packing; do
 "$build/target" "$group" > "$build/group.log"
 cat "$build/group.log"
 grep -E "^EXECUTED $group assertions=[1-9][0-9]*$" "$build/group.log" >/dev/null
done
compile examples/disk_meshlets.cpp ${PRODUCTION_SOURCES} -o "$build/example"
"$build/example" "$build/out" > "$build/example.log"
cat "$build/example.log"
[ -s "$build/example.log" ]
[ -s "$build/out/result.obj" ]
echo TARGET_OK
