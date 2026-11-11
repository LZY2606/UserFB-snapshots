#!/bin/sh
#
# Regression checks for the msdfgen core distance-field pipeline.
#
#   sh tests/check-distance-field.sh
#
# What this does:
#   1. Compiles ONLY the core library together with an independent test driver
#      (no font loading, no extensions, no GPU backend).
#   2. Checks generateSDF / generateMSDF / generateMTSDF against hand-derived
#      geometry references, and renderSDF against an independently computed
#      bilinear/median reconstruction on hand-built 2x2 and 3x2 textures.
#      Per-case maximum numeric errors and the first sign mismatch are
#      printed; sharp-corner and hole PGM reconstructions are written under
#      tests/distance-field-checks/pgm/.
#   3. In three separate temporary build copies it injects one regression
#      each - median replaced by mean, missing half-pixel center offset,
#      reversed Y interpretation - and proves every one is caught, then
#      deletes the copies.
#
# A healthy library must pass step 2 and all three "must fail" mutants.
# The reference implementation in the test driver never calls the production
# scanline, distance selector or rasterization code.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TEST_SRC="$SCRIPT_DIR/distance-field-checks/distance-field-test.cpp"
PGM_DIR="$SCRIPT_DIR/distance-field-checks/pgm"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:-"-O2 -std=c++11 -DMSDFGEN_PUBLIC= -DMSDFGEN_USE_CPP11"}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/msdf-dfcheck.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

mkdir -p "$PGM_DIR"

echo "== Compiling core + independent test driver =="
"$CXX" $CXXFLAGS -I"$ROOT" "$ROOT"/core/*.cpp "$TEST_SRC" -o "$WORK/dftest"

echo
echo "== Running checks against the unmodified library =="
"$WORK/dftest" "$PGM_DIR"

# ---------------------------------------------------------------------------
# Mutation harness: build a full copy of core per regression so the mutation
# cannot touch the real source tree, and require the checks to reject it.
#
# $1 = mutation name, $2 = file to edit, $3 = sed program, $4 = marker that
# must appear in the edited file (proves the replacement actually happened).
run_mutant() {
    name=$1
    file=$2
    sed_prog=$3
    marker=$4

    dir="$WORK/mut-$name"
    mkdir -p "$dir"
    cp "$ROOT/msdfgen.h" "$dir/"
    cp -R "$ROOT/core" "$dir/core"
    target="$dir/core/$file"
    sed "$sed_prog" "$target" > "$target.tmp"
    mv "$target.tmp" "$target"
    if ! grep -F -q "$marker" "$target"; then
        echo "ERROR: mutation '$name' did not apply (marker not found in $file)" >&2
        exit 1
    fi

    echo
    echo "== Mutant '$name': building and expecting a FAILURE =="
    "$CXX" $CXXFLAGS -I"$dir" "$dir"/core/*.cpp "$TEST_SRC" -o "$dir/dftest"
    log="$dir/run.log"
    if "$dir/dftest" "$dir" > "$log" 2>&1; then
        cat "$log"
        echo "ERROR: mutant '$name' passed the checks; the checks are too weak" >&2
        exit 1
    fi
    echo "mutant '$name' rejected as expected. Failing sample(s):"
    grep '^FAIL' "$log" | head -3 | sed 's/^/  /'
    grep '^\[summary\]' "$log" | sed 's/^/  /'
}

# Regression 1: RGB median silently replaced by the arithmetic mean.
run_mutant mean render-sdf.cpp \
    's/median(sd\[0\], sd\[1\], sd\[2\])/((sd[0]+sd[1]+sd[2])\/3.0f)/g' \
    '(sd[0]+sd[1]+sd[2])/3.0f'

# Regression 2: output pixel centers lose their +0.5 half-pixel offset.
# In a POSIX BRE the '+' is a literal plus sign (the source's x+.5 / y+.5).
run_mutant nohalf render-sdf.cpp \
    's/scale\*Point2(x+\.5, y+\.5)/scale*Point2(x, y)/g' \
    'scale*Point2(x, y)'

# Regression 3: the Y axis of sampled positions is interpreted backwards.
run_mutant yflip bitmap-interpolation.hpp \
    's/pos\.y = clamp(pos\.y, double(bitmap\.height));/pos.y = clamp(double(bitmap.height)-pos.y, double(bitmap.height));/' \
    'double(bitmap.height)-pos.y'

echo
echo "All normal checks passed and all three injected regressions were detected."
echo "PGM reconstructions: $PGM_DIR/gen.arrow.pgm (sharp corners), $PGM_DIR/gen.hole.pgm (hole)"
