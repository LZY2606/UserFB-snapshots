#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/wavpack-build.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
mode=${1:-baseline}
if [ "$mode" = target ];then for f in float_wire_test.c float_wire_model.c float_wire_model.h float_wire_packets.c;do test -s "$root/tests/$f";done;fi
cmake -S "$root" -B "$b/build" -DWAVPACK_BUILD_PROGRAMS=OFF -DWAVPACK_ENABLE_ASM=OFF -DWAVPACK_ENABLE_THREADS=OFF -DWAVPACK_ENABLE_DSD=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF > "$b/config.log" 2>&1 || { cat "$b/config.log";exit 1; }
cmake --build "$b/build" --target wavpack -j 4 > "$b/build.log" 2>&1 || { cat "$b/build.log";exit 1; }
compile(){ cc -std=c99 -O1 -I "$root/include" -I "$root/src" -I "$root/task_support" "$@" "$b/build/libwavpack.a" -lm -o "$b/probe"; }
cd "$b"
if [ "$mode" = baseline ];then compile "$root/task_support/baseline.c";./probe
elif [ "$mode" = target ];then
 compile "$root/task_support/driver.c" "$root/tests/float_wire_test.c" "$root/tests/float_wire_model.c" "$root/tests/float_wire_packets.c";./probe
 for kind in negative_zero nan;do
  awk -f "$root/task_support/alter.awk" "$kind" "$root/src/unpack_floats.c" > "$b/mutant.c"
  compile "$root/task_support/driver.c" "$root/tests/float_wire_test.c" "$root/tests/float_wire_model.c" "$root/tests/float_wire_packets.c" "$b/mutant.c"
  set +e
  ./probe > "$b/mutant.log" 2>&1
  rc=$?
  set -e
  cat "$b/mutant.log"
  test "$rc" -eq 42
  grep -q '^FAIL ' "$b/mutant.log"
  echo "float semantic change $kind detected"
 done
else exit 2;fi
