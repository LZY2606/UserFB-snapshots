#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/aac-layout-build.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
mode=${1:-baseline}
if [ "$mode" = baseline ]; then
 set -- "$root/task_support/baseline.c"
elif [ "$mode" = target ]; then
 test -s "$root/tests/layout_update.c"
 set -- "$root/task_support/driver.c" "$root"/tests/layout_*.c
else exit 2; fi
cmake -S "$root" -B "$b/build" -DBUILD_SHARED_LIBS=OFF -DFAAD_BUNDLED_MODE=ON > "$b/config.log" 2>&1 || { tail -30 "$b/config.log"; exit 1; }
cmake --build "$b/build" --target faad -j 4 > "$b/build.log" 2>&1 || { tail -30 "$b/build.log"; exit 1; }
cc -std=c99 -O1 -DHAVE_INTTYPES_H=1 -I "$root" -I "$root/libfaad" -I "$root/include" -I "$root/task_support" "$@" "$root/task_support/aac_fixture.c" "$b/build/libfaad.a" -lm -o "$b/probe"
cd "$b"
set +e
./probe > "$b/output.log" 2>&1
status=$?
set -e
cat "$b/output.log"
[ "$status" -eq 0 ] || exit "$status"
if [ "$mode" = target ]; then
 grep -Eq '^TARGET checks=[1-9][0-9]*$' "$b/output.log"
fi
