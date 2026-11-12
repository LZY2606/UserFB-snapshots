#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/ufbx_baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -I. -c ufbx.c -o "$b/ufbx.o"
cc -std=c99 -O2 -I. checks/baseline.c "$b/ufbx.o" -lm -o "$b/base"
"$b/base"
cc -std=c99 -O1 -I. test/runner.c "$b/ufbx.o" -lm -o "$b/old"
for name in maya_dual_quaternion maya_dual_quaternion_scale maya_dq_weights; do
 "$b/old" --test "$name" --data data --no-fuzz > "$b/$name.out"
 cat "$b/$name.out"
 grep -F "$name: OK" "$b/$name.out" >/dev/null
done
echo UFBX_OLD_SKIN_TESTS_OK
