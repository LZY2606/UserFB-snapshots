#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:?baseline or target}
case "$mode" in baseline|target) ;; *) exit 2;; esac
build=$(mktemp -d "${TMPDIR:-/tmp}/uf-par.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I . -c support/implementation.c -o "$build/par.o"
cc -std=c99 -O2 -I . support/baseline.c "$build/par.o" -lm -o "$build/baseline"
"$build/baseline" | tee "$build/baseline.log"
grep -Fxq 'MESH_BASELINE cube_volume=24 torus=117/192 welded=96/192 clone=independent' "$build/baseline.log"
# This upstream target only uses libc and libm; other test targets need curl.
cc -std=c99 -O2 -I . -I test test/test_shapes.c test/console-colors.c -lm -o "$build/old"
mkdir "$build/build"
(cd "$build" && ./old) > "$build/old.log" 2>&1
cat "$build/old.log"
count=$(grep -c '✓' "$build/old.log")
[[ "$count" == 16 ]] || { echo "OLD_CASE_COUNT $count" >&2; exit 9; }
if grep -q '✖' "$build/old.log"; then exit 9; fi
echo 'MESH_OLD cases=16 DONE'
[[ "$mode" == target ]] || exit 0
[[ -s support/rounded_target.c ]] || { echo 'MISSING_TARGET support/rounded_target.c' >&2; exit 10; }
[[ -s examples/rounded.c ]] || { echo 'MISSING_EXAMPLE examples/rounded.c' >&2; exit 12; }
for bits in 16 32; do
  flags=(-DPAR_SHAPES_T=uint16_t)
  [[ "$bits" == 16 ]] || flags=(-DPAR_SHAPES_T=uint32_t)
  cc -std=c99 -O2 -I . "${flags[@]}" -c support/implementation.c -o "$build/par$bits.o"
  cc -std=c99 -O2 -I . -I support "${flags[@]}" support/rounded_target.c support/driver.c -lm -o "$build/target$bits"
  echo "MESH_TARGET_COMPILED bits=$bits"
  for group in patches topology error; do
    set +e
    "$build/target$bits" "$group" > "$build/$bits-$group.log" 2>&1
    status=$?
    set -e
    cat "$build/$bits-$group.log"
    [[ "$status" == 0 ]] || exit "$status"
    grep -Eq "^UNIT_GROUP $group checks=[1-9][0-9]* DONE$" "$build/$bits-$group.log" || { echo "INCOMPLETE $bits $group" >&2; exit 11; }
  done
  cc -std=c99 -O2 -I . "${flags[@]}" examples/rounded.c "$build/par$bits.o" -lm -o "$build/example$bits"
  mkdir "$build/out$bits"
  "$build/example$bits" "$build/out$bits"
  [[ -s "$build/out$bits/rounded.obj" ]] || { echo 'NO_EXAMPLE_OBJ' >&2; exit 12; }
  echo "MESH_INDEX bits=$bits groups=3 DONE"
done
echo 'MESH_ROUNDED_TARGET indices=2 groups=6 DONE'
