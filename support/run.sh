#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
mode=${1:-baseline}
compile() { ${CC:-cc} -std=c99 -O1 -I. -Isupport "$@" -lm; }
compile support/baseline.c -o "$build/baseline"
"$build/baseline"
if [ "$mode" = baseline ]; then exit 0; fi
[ "$mode" = target ] || exit 2
[ -s support/target.c ] || { echo 'missing support/target.c' >&2; exit 2; }
[ -s examples/shell.c ] || { echo 'missing examples/shell.c' >&2; exit 2; }
for bits in 16 32; do
 indexflags=""
 if [ "$bits" = 32 ]; then indexflags="-DPAR_SHAPES_T=uint32_t"; fi
 compile $indexflags support/target.c support/driver.c -o "$build/target"
 for group in skins rim topology reject; do
  "$build/target" "$group" > "$build/group.log"
  cat "$build/group.log"
  grep -E "^EXECUTED $group assertions=[1-9][0-9]*$" "$build/group.log" >/dev/null
 done
 echo "INDEX_WIDTH $bits"
done
compile examples/shell.c ${PRODUCTION_SOURCES:-} -o "$build/example"
"$build/example" "$build/out" > "$build/example.log"
cat "$build/example.log"
[ -s "$build/example.log" ]
[ -s "$build/out/result.obj" ]
echo TARGET_OK
