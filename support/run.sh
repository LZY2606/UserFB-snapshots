#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
mode=${1:-baseline}
PRODUCTION_SOURCES="src/tinyspline.c src/parson.c"
compile() { ${CC:-cc} -std=c99 -O1 -I. -Isrc -Isupport "$@" -lm; }
compile support/baseline.c ${PRODUCTION_SOURCES} -o "$build/baseline"
"$build/baseline"
if [ "$mode" = baseline ]; then exit 0; fi
[ "$mode" = target ] || exit 2
[ -s support/target.c ] || { echo 'missing support/target.c' >&2; exit 2; }
[ -s src/tinyspline_surface.c ] || { echo "missing src/tinyspline_surface.c" >&2; exit 2; }
[ -s examples/loft.c ] || { echo 'missing examples/loft.c' >&2; exit 2; }
compile support/target.c support/driver.c src/tinyspline_surface.c ${PRODUCTION_SOURCES:-} -o "$build/target"
for group in sections rational derivatives ownership; do
 "$build/target" "$group" > "$build/group.log"
 cat "$build/group.log"
 grep -E "^EXECUTED $group assertions=[1-9][0-9]*$" "$build/group.log" >/dev/null
done
compile examples/loft.c src/tinyspline_surface.c ${PRODUCTION_SOURCES:-} -o "$build/example"
"$build/example" "$build/out" > "$build/example.log"
cat "$build/example.log"
[ -s "$build/example.log" ]
[ -s "$build/out/result.obj" ]
echo TARGET_OK
