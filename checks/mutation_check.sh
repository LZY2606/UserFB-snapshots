#!/bin/bash
# Mutation verification for the watertight triangle intersector.
#
# The production source is never modified. Two temporary copies are made in
# $TMPDIR and each gets exactly one source change:
#   no-edge-fallback : disable the double-precision edge fallback that runs
#                      when U/V/W is zero; the first-pass edge weights remain.
#   packed-intersector: make TriangleIntersector address vertices with a hard
#                      3*sizeof(T) stride instead of the caller stride.
# TriangleMesh and TriangleSAHPred keep using the caller-provided stride in
# every copy. A copy is only accepted if it compiles, still passes the
# existing baseline and legacy regressions, and then fails the new geometric
# probe with a CONTENT_FAILURE (exit status 3) carrying a DETECTED marker.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
CXX=${CXX:-c++}

work=$(mktemp -d "${TMPDIR:-/tmp}/nanort-mut.XXXXXX")
trap 'rm -rf "$work"' EXIT

# 1. Benign regressions must pass on the untouched production source first.
bash checks/baseline.sh >/dev/null
bash checks/legacy.sh   >/dev/null

run_copy() {
  local name=$1
  local dst="$work/$name"
  mkdir -p "$dst"
  cp nanort.h "$dst/nanort.h"

  if [ "$name" = "no-edge-fallback" ]; then
    # Neutralise only the fallback guard; the block is simply never entered.
    perl -0777 -pi -e 's/if \(U == static_cast<T>\(0\.0\) \|\| V == static_cast<T>\(0\.0\) \|\|\s*W == static_cast<T>\(0\.0\)\) \{/if (false) {/s' "$dst/nanort.h"
    grep -q 'if (false) {' "$dst/nanort.h" || { echo "RUNTIME ERROR: no-edge-fallback edit not applied"; exit 1; }
  elif [ "$name" = "packed-intersector" ]; then
    # Only the intersector's f0/f1/f2 address computation is hardened.
    perl -0777 -pi -e 's/get_vertex_addr\(vertices_, (f[012]) \+ 0, vertex_stride_bytes_\)/get_vertex_addr(vertices_, $1 + 0, 3 * sizeof(T))/g' "$dst/nanort.h"
    [ "$(grep -c 'get_vertex_addr(vertices_, f[012] + 0, 3 \* sizeof(T))' "$dst/nanort.h")" -eq 3 ] \
      || { echo "RUNTIME ERROR: packed-intersector edit not applied"; exit 1; }
    # Mesh and SAHPred must still honour the caller stride. The SAHPred uses
    # the <T>-qualified i0/i1/i2 sites; TriangleMesh uses the vertex sites.
    grep -q 'get_vertex_addr<T>(vertices_, i0, vertex_stride_bytes_)' "$dst/nanort.h" \
      || { echo "RUNTIME ERROR: SAHPred stride was altered"; exit 1; }
    grep -q 'get_vertex_addr(vertices_, vertex, vertex_stride_bytes_)' "$dst/nanort.h" \
      || { echo "RUNTIME ERROR: TriangleMesh stride was altered"; exit 1; }
  else
    echo "RUNTIME ERROR: unknown mutation $name"; exit 1
  fi

  # 2. The copy compiles, and the pre-existing regressions still pass there.
  "$CXX" -std=c++17 -O2 -I"$dst" -I"$root" checks/baseline.cpp -o "$work/base"
  "$work/base" >/dev/null
  "$CXX" -std=c++17 -O2 -I"$dst" -I"$root" \
      test/regression/possible-accuracy-problem-30/main.cc -o "$work/legacy"
  "$work/legacy" >/dev/null
  "$work/legacy" tiny >/dev/null

  # 3. The new geometric probe must turn this exact change into a content
  #    failure. Contraction is disabled so separate multiply/subtract edge
  #    weights are modelled deterministically (no target-dependent FMA).
  "$CXX" -std=c++17 -O2 -ffp-contract=off -I"$dst" -I"$root" -I"$root"/checks \
      checks/mutation_driver.cpp -o "$work/driver"

  set +e
  "$work/driver" "$name" >"$work/out" 2>&1
  local rc=$?
  set -e
  if [ "$rc" -ne 3 ]; then
    echo "RUNTIME ERROR: $name probe did not produce CONTENT_FAILURE (rc=$rc)"
    cat "$work/out"
    exit 1
  fi
  grep -q "DETECTED $name" "$work/out" \
    || { echo "RUNTIME ERROR: missing DETECTED marker for $name"; cat "$work/out"; exit 1; }
  grep -q "first expected/actual" "$work/out" \
    || { echo "RUNTIME ERROR: missing expected/actual diff for $name"; cat "$work/out"; exit 1; }
  cat "$work/out"
}

run_copy no-edge-fallback
run_copy packed-intersector
echo "MUTATION_CHECK_OK detected=2"
