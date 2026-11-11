#!/bin/sh
#----------------------------------------------------------------------------
# Windowless compositing regression driver.
#
#   sh checks/check_compositing.sh
#       Build the regression in a temporary directory against the in-tree
#       agg-src/include headers, run it, and leave the enlarged PPM edge
#       previews behind.  Nothing is written into the source tree.
#
#   sh checks/check_compositing.sh --mutations
#       Hand over to checks/run_mutation_checks.sh: confirm the unmodified
#       library passes, then apply four single-point faults to isolated
#       source copies and require each one to be caught by a numeric or
#       sentinel assertion.
#
# Only an existing C++ compiler and the standard library are required.
# No sample application or window is started.
#----------------------------------------------------------------------------
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:-"-std=c++14 -O2"}
TEST_SRC="$ROOT/checks/compositing_test.cpp"
SRC_INCLUDE="$ROOT/agg-src/include"

usage()
{
    echo "usage: sh $0 [--mutations] [--out DIR]"
    echo "  (no arguments)  build and run the compositing regression"
    echo "  --mutations     run the isolated single-point failure checks"
    echo "  --out DIR       write the enlarged PPM previews to DIR"
    echo "                  (default: \$TMPDIR/agg-compositing-previews)"
}

MUTATIONS=0
OUT_DIR=${AGG_COMPOSITING_OUT:-${TMPDIR:-/tmp}/agg-compositing-previews}

while [ $# -gt 0 ]; do
    case "$1" in
        --mutations)
            MUTATIONS=1
            shift
            ;;
        --out)
            if [ $# -lt 2 ]; then
                usage >&2
                exit 2
            fi
            OUT_DIR=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$MUTATIONS" -eq 1 ]; then
    exec sh "$ROOT/checks/run_mutation_checks.sh"
fi

if [ ! -f "$TEST_SRC" ]; then
    echo "check_compositing: missing $TEST_SRC" >&2
    exit 2
fi
if [ ! -d "$SRC_INCLUDE" ]; then
    echo "check_compositing: missing $SRC_INCLUDE" >&2
    exit 2
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/agg-compositing.XXXXXX") || exit 2
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

echo "== compositing regression =="
echo "root: $ROOT"
echo "compiler: $CXX"
echo "work: $WORK"

# shellcheck disable=SC2086
$CXX $CXXFLAGS -Wall -Wextra \
    -Wno-unused-variable -Wno-unused-parameter \
    -I"$SRC_INCLUDE" "$TEST_SRC" -o "$WORK/compositing_test" \
    > "$WORK/compile.log" 2>&1
if [ $? -ne 0 ]; then
    echo "compile failed:" >&2
    sed -n '1,40p' "$WORK/compile.log" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"
if ! "$WORK/compositing_test" "$OUT_DIR" > "$WORK/run.log" 2>&1; then
    echo "regression failed:" >&2
    grep -m 10 '^FAIL' "$WORK/run.log" >&2 || true
    tail -3 "$WORK/run.log" >&2
    echo "full log: $WORK/run.log (temporary)" >&2
    exit 1
fi

tail -4 "$WORK/run.log"
echo
echo "enlarged previews (retained):"
echo "  $OUT_DIR/compositing_color.ppm"
echo "  $OUT_DIR/compositing_alpha.ppm"
exit 0
