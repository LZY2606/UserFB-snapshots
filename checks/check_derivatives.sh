#!/usr/bin/env bash
#
#  checks/check_derivatives.sh
#
#  Builds the fixed source tree and executes checks/derivatives.cpp in
#  double precision on the CPU with OPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES.
#  Three independent temporary copies are then built; each copy changes
#  exactly one production behavior:
#
#    derivative-scale : subpatch first-derivative depth scale is omitted
#    mixed-sign       : one mixed second derivative is sign-reversed
#    fvar-index       : one side's UV values borrow the adjacent side identity
#
#  Each copy must compile, produce finite positions, and be rejected by the
#  same numerical checks.  Only when all three normal groups pass and all
#  three mutated copies are detected is OPENSUBDIV_DERIVATIVES_OK emitted.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-.}/opensubdiv-derivatives.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

die() {
  echo "ERROR: $*" >&2
  exit 1
}

CMAKE_FLAGS=(
  -DNO_EXAMPLES=ON -DNO_TUTORIALS=ON -DNO_REGRESSION=ON -DNO_DOC=ON
  -DNO_OMP=ON -DNO_TBB=ON -DNO_CUDA=ON -DNO_OPENCL=ON -DNO_OPENGL=ON
  -DNO_METAL=ON -DNO_PTEX=ON -DNO_TESTS=ON -DNO_MACOS_FRAMEWORK=ON
  -DBUILD_SHARED_LIBS=OFF
  -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES=ON
)

configure_and_build() {
  local src="$1" bld="$2"
  cmake -S "$src" -B "$bld" "${CMAKE_FLAGS[@]}" > "$bld".configure.log 2>&1 || {
    cat "$bld".configure.log >&2
    die "cmake configure failed in $src"
  }
  cmake --build "$bld" --target osd_static_cpu -j 4 > "$bld".build.log 2>&1 || {
    cat "$bld".build.log >&2
    die "library build failed in $src"
  }
}

build_checker() {
  local src="$1" bld="$2"
  c++ -std=c++17 -O2 -I "$src" "$src/checks/derivatives.cpp" \
      "$bld/lib/libosdCPU.a" -o "$bld/derivatives" > "$bld".compile.log 2>&1 || {
    cat "$bld".compile.log >&2
    die "checks/derivatives.cpp failed to compile in $src"
  }
}

copy_tree() {
  local dst="$1"
  mkdir -p "$dst"
  tar -C "$ROOT" --exclude='./.git' --exclude='./.dbg-build' \
      --exclude='./.reg-build' --exclude='./.claude' -cf - . | tar -C "$dst" -xf -
}

mutate_derivative_scale() {
  local src="$1"
  local f="$src/opensubdiv/far/patchBasis.cpp"
  sed -i.bak \
    -e 's|REAL d1Scale = derivSign \* (REAL)(1 << param.GetDepth());|REAL d1Scale = derivSign;|' \
    -e 's|REAL d2Scale = d1Scale \* d1Scale;|REAL d2Scale = (REAL)((1 << param.GetDepth()) * (1 << param.GetDepth()));|' \
    "$f"
  rm -f "$f.bak"
  grep -Fq 'REAL d1Scale = derivSign;' "$f" || die "derivative-scale mutation did not apply"
  grep -Fq 'REAL d2Scale = (REAL)((1 << param.GetDepth()) * (1 << param.GetDepth()));' "$f" \
    || die "derivative-scale second-derivative isolation did not apply"
}

mutate_mixed_sign() {
  local src="$1"
  local f="$src/opensubdiv/far/patchBasis.cpp"
  sed -i.bak 's|wDst\[i\] \*= d2Scale;|wDst[i] *= -d2Scale;|' "$f"
  rm -f "$f.bak"
  [[ $(grep -Fc 'wDst[i] *= -d2Scale;' "$f") -eq 1 ]] \
    || die "mixed-sign mutation did not apply exactly once"
}

mutate_fvar_index() {
  local src="$1"
  local f="$src/opensubdiv/bfr/refinerSurfaceFactory.cpp"
  local count line
  count=$(grep -Fc 'GetFaceFVarValues(baseFace, fvarChannel);' "$f")
  [[ $count -eq 1 ]] || die "fvar-index mutation anchor is not unique"
  line=$(grep -Fn 'GetFaceFVarValues(baseFace, fvarChannel);' "$f" | head -1 | cut -d: -f1)
  sed -i.bak "${line}s|GetFaceFVarValues(baseFace, fvarChannel);|GetFaceFVarValues((baseFace == 6) ? (Far::Index)5 : baseFace, fvarChannel);|" "$f"
  rm -f "$f.bak"
  grep -Fq 'GetFaceFVarValues((baseFace == 6) ? (Far::Index)5 : baseFace, fvarChannel);' "$f" \
    || die "fvar-index mutation did not apply"
}

LOG="$WORK/final.log"
: > "$LOG"

# ---------------------------------------------------------------------------
#  Normal (fixed) source tree.
# ---------------------------------------------------------------------------
NORMAL_BUILD="$WORK/build-normal"
configure_and_build "$ROOT" "$NORMAL_BUILD"
build_checker "$ROOT" "$NORMAL_BUILD"

"$NORMAL_BUILD/derivatives" > "$WORK/normal.log" 2>&1 || {
  cat "$WORK/normal.log" >&2
  die "normal numerical checks reported a hard failure"
}

for name in derivative-reference stencil-identity fvar-sides; do
  count=$(grep -Ec "^PASS ${name} cases=[1-9][0-9]*$" "$WORK/normal.log" || true)
  [[ $count -eq 1 ]] || {
    cat "$WORK/normal.log" >&2
    die "normal run did not emit PASS ${name} cases=N (N>0) exactly once"
  }
done
cat "$WORK/normal.log" >> "$LOG"

# ---------------------------------------------------------------------------
#  Three independent temporary copies, one behavior change each.
# ---------------------------------------------------------------------------
names=(derivative-scale mixed-sign fvar-index)
groups=(derivative-reference derivative-reference fvar-sides)
defects=(derivative-scale mixed-sign fvar-index)
mutators=(mutate_derivative_scale mutate_mixed_sign mutate_fvar_index)

for i in 0 1 2; do
  name="${names[$i]}"
  group="${groups[$i]}"
  defect="${defects[$i]}"
  dst="$WORK/copy-$name"
  copy_tree "$dst"
  "${mutators[$i]}" "$dst"
  bld="$WORK/build-$name"
  configure_and_build "$dst" "$bld"
  build_checker "$dst" "$bld"

  "$bld/derivatives" --group="$group" > "$WORK/$name.log" 2>&1 || {
    cat "$WORK/$name.log" >&2
    die "copy $name failed to run (compile error, crash, or hard failure)"
  }

  # The copies must still produce finite positions ...
  grep -Eq "^finite_positions group=${group} cases=[1-9][0-9]*$" "$WORK/$name.log" || {
    cat "$WORK/$name.log" >&2
    die "copy $name did not report finite positions for its executed cases"
  }
  # ... and the same numerical checks must clearly reject the changed behavior.
  count=$(grep -Ec "^DETECTED ${defect} cases=[1-9][0-9]*$" "$WORK/$name.log" || true)
  [[ $count -eq 1 ]] || {
    cat "$WORK/$name.log" >&2
    die "copy $name was not detected as ${defect} (missing positive DETECTED line)"
  }

  grep -E '^FAIL ' "$WORK/$name.log" >> "$LOG" || true
  grep -E "^DETECTED ${defect} cases=[1-9][0-9]*$" "$WORK/$name.log" >> "$LOG"
done

# ---------------------------------------------------------------------------
#  Final accounting.  Each group must occur exactly once, with a real count.
# ---------------------------------------------------------------------------
for name in derivative-reference stencil-identity fvar-sides; do
  [[ $(grep -Ec "^PASS ${name} cases=[1-9][0-9]*$" "$LOG" || true) -eq 1 ]] \
    || die "final log is missing PASS ${name} cases=N"
done
for name in derivative-scale mixed-sign fvar-index; do
  [[ $(grep -Ec "^DETECTED ${name} cases=[1-9][0-9]*$" "$LOG" || true) -eq 1 ]] \
    || die "final log is missing DETECTED ${name} cases=N"
done
if grep -Eiq '(^|[[:space:]])(SKIP|SKIPPED)([[:space:]]|$)' "$LOG"; then
  die "skipped cases are not accepted"
fi

cat "$LOG"
echo "OPENSUBDIV_DERIVATIVES_OK"
