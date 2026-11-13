#!/bin/sh
set -eu
mode=${1:-baseline}
if [ "$mode" = target ];then test -s "$(dirname "$0")/../tests/caf_layout_test.c";fi
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/caf-build.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
export HTTP_PROXY=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 ALL_PROXY=http://127.0.0.1:9
cmake -S "$root" -B "$b/build" -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF > "$b/config.log" 2>&1 || { cat "$b/config.log"; exit 1; }
cmake --build "$b/build" --target sndfile -j 4 > "$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cd "$b"
if [ "$mode" = baseline ]; then
 cc -std=c99 -I "$root/include" "$root/task_support/baseline.c" "$b/build/libsndfile.a" -lm -o probe
elif [ "$mode" = target ]; then
 test -s "$root/tests/caf_layout_test.c"
 cc -std=c99 -I "$root/include" -I "$root/task_support" "$root/task_support/driver.c" "$root/tests/caf_layout_test.c" "$b/build/libsndfile.a" -lm -o probe
else exit 2; fi
./probe
