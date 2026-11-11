#!/bin/sh
#----------------------------------------------------------------------------
# Isolated single-point failure checks for the compositing regression.
#
# Each mutation is applied to its own temporary copy of agg-src/include; the
# original source tree is never modified.  The same checks/compositing_test.cpp
# regression runs against the mutant.  A mutation only counts as detected when
# the test terminates with a numeric channel assertion or a sentinel
# assertion (a line starting with "FAIL").  Compile failures, timeouts and
# crashes without such an assertion do not count.
#
# Only a C++ compiler, the standard library and POSIX shell tools are used.
#----------------------------------------------------------------------------
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:-"-std=c++14 -O2"}
RUN_LIMIT=${RUN_LIMIT:-60}
TEST_SRC="$ROOT/checks/compositing_test.cpp"
SRC_INCLUDE="$ROOT/agg-src/include"

if [ ! -f "$TEST_SRC" ]; then
    echo "mutation-check: missing $TEST_SRC" >&2
    exit 2
fi
if [ ! -d "$SRC_INCLUDE" ]; then
    echo "mutation-check: missing $SRC_INCLUDE" >&2
    exit 2
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/agg-compositing-mut.XXXXXX") || exit 2
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

FAILED=0

# Replace exactly one line; fail loudly if the anchor is absent or
# ambiguous.  Old/new are compared without indentation; the original
# indentation of the matched line is preserved.
apply_line()
{
    _file=$1
    OLD=$2
    NEW=$3
    export OLD NEW
    awk '
        BEGIN { old = ENVIRON["OLD"]; new = ENVIRON["NEW"] }
        {
            cr = ""
            line = $0
            if (substr(line, length(line), 1) == "\r") {
                cr = "\r"; line = substr(line, 1, length(line) - 1)
            }
            match(line, /^[ \t]*/)
            ind  = substr(line, 1, RLENGTH)
            body = substr(line, RLENGTH + 1)
            if (body == old) { print ind new cr; n++; next }
            print
        }
        END { if (n != 1) exit 42 }
    ' "$_file" > "$_file.new"
    _rc=$?
    if [ $_rc -ne 0 ]; then
        rm -f "$_file.new"
        return 1
    fi
    mv "$_file.new" "$_file"
    return 0
}

# Replace one line, but only inside the blender_rgba_pre definition, so the
# identical alpha line in blender_rgba is left untouched.
apply_alpha_line()
{
    _file=$1
    OLD=$2
    NEW=$3
    export OLD NEW
    awk '
        BEGIN { old = ENVIRON["OLD"]; new = ENVIRON["NEW"] }
        /^    struct blender_rgba_pre/ { in_pre = 1 }
        {
            cr = ""
            line = $0
            if (substr(line, length(line), 1) == "\r") {
                cr = "\r"; line = substr(line, 1, length(line) - 1)
            }
            match(line, /^[ \t]*/)
            ind  = substr(line, 1, RLENGTH)
            body = substr(line, RLENGTH + 1)
            if (in_pre && body == old) {
                print ind new cr; in_pre = 0; found++; next
            }
            print
        }
        END { if (found != 1) exit 43 }
    ' "$_file" > "$_file.new"
    _rc=$?
    if [ $_rc -ne 0 ]; then
        rm -f "$_file.new"
        return 1
    fi
    mv "$_file.new" "$_file"
    return 0
}

run_limited()
{
    _log=$1
    _mark=$2
    shift 2
    rm -f "$_mark"
    "$@" > "$_log" 2>&1 &
    _pid=$!
    (
        sleep "$RUN_LIMIT"
        if kill -0 "$_pid" 2>/dev/null; then
            : > "$_mark"
            kill -9 "$_pid" 2>/dev/null
        fi
    ) &
    _guard=$!
    wait "$_pid"
    _rc=$?
    kill "$_guard" 2>/dev/null
    wait "$_guard" 2>/dev/null
    return $_rc
}

echo "== compositing mutation checks =="
echo "root: $ROOT"
echo "compiler: $CXX"
echo "work: $WORK"
echo

#---------------------------------------------------------------------------
# 0. Confirm the unmodified library passes first.
#---------------------------------------------------------------------------
BASE="$WORK/baseline"
mkdir -p "$BASE/out"
cp -R "$SRC_INCLUDE" "$BASE/include" || exit 2
echo "[baseline] compiling"
# shellcheck disable=SC2086
$CXX $CXXFLAGS -I"$BASE/include" "$TEST_SRC" -o "$BASE/test" \
    > "$BASE/compile.log" 2>&1
if [ $? -ne 0 ]; then
    echo "[baseline] COMPILE FAILED" >&2
    sed -n '1,40p' "$BASE/compile.log" >&2
    exit 2
fi
run_limited "$BASE/run.log" "$BASE/timed_out" "$BASE/test" "$BASE/out"
BASE_RC=$?
if [ -f "$BASE/timed_out" ]; then
    echo "[baseline] TIMEOUT after ${RUN_LIMIT}s" >&2
    exit 2
fi
if [ $BASE_RC -ne 0 ] || ! grep -q "^ALL CHECKS PASSED" "$BASE/run.log"; then
    echo "[baseline] FAILED (exit $BASE_RC); the unmodified library must pass" >&2
    sed -n '1,40p' "$BASE/run.log" >&2
    exit 2
fi
echo "[baseline] PASS ($(grep '^==== compositing regression' "$BASE/run.log" | tail -1))"
echo

#---------------------------------------------------------------------------
# Mutation descriptors.  Each entry: name | file | applier | old | new
#---------------------------------------------------------------------------
NAME1="premultiplied source multiplied by alpha again"
NAME2="cover reused (applied twice)"
NAME3="destination alpha lost during compositing"
NAME4="clipped span does not advance cover start"

PKG="$WORK/pkg"
mkdir -p "$PKG"

run_mutation()
{
    _n=$1
    _name=$2
    _file_rel=$3
    _applier=$4
    _old=$5
    _new=$6

    _dir="$PKG/m$_n"
    mkdir -p "$_dir/out"
    cp -R "$SRC_INCLUDE" "$_dir/include" || return 2

    echo "[$_n] $_name"
    echo "     anchor: $_file_rel"

    _file="$_dir/include/$_file_rel"
    if [ ! -f "$_file" ]; then
        echo "     RESULT: HARNESS ERROR (file not found)"
        return 2
    fi

    if "$_applier" "$_file" "$_old" "$_new"; then
        :
    else
        echo "     RESULT: HARNESS ERROR (mutation anchor not found exactly once)"
        return 2
    fi

    # shellcheck disable=SC2086
    $CXX $CXXFLAGS -I"$_dir/include" "$TEST_SRC" -o "$_dir/test" \
        > "$_dir/compile.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "     RESULT: NOT DETECTED (compile failure; not counted)"
        sed -n '1,12p' "$_dir/compile.log" | sed 's/^/       /'
        return 1
    fi

    run_limited "$_dir/run.log" "$_dir/timed_out" "$_dir/test" "$_dir/out"
    _rc=$?
    if [ -f "$_dir/timed_out" ]; then
        echo "     RESULT: NOT DETECTED (timeout after ${RUN_LIMIT}s; not counted)"
        return 1
    fi

    _first=$(grep -m 1 '^FAIL' "$_dir/run.log" 2>/dev/null)
    if [ -z "$_first" ]; then
        if [ $_rc -eq 0 ]; then
            echo "     RESULT: NOT DETECTED (regression passed)"
        else
            echo "     RESULT: NOT DETECTED (terminated with exit $_rc but no numeric/sentinel assertion)"
        fi
        return 1
    fi

    _case=$(printf '%s\n' "$_first" | awk '{print $2}')
    _chan=$(printf '%s\n' "$_first" | awk '
        match($0, /channel=[RGBA]/) {
            print substr($0, RSTART + 8, 1)
        }')
    [ -z "$_chan" ] && _chan="- (sentinel/untouched assertion)"
    echo "     RESULT: DETECTED (exit $_rc)"
    echo "     first failing case: $_case"
    echo "     first failing channel: $_chan"
    echo "     assertion: $_first"
    return 0
}

# 1. The pre-multiplied entry point multiplies its already-premultiplied
#    source channels by alpha one more time.
run_mutation 1 "$NAME1" "agg_pixfmt_rgba.h" apply_line \
"comp_op_table_rgba<ColorT, Order>::g_comp_op_func[op](p, r, g, b, a, cover);" \
"comp_op_table_rgba<ColorT, Order>::g_comp_op_func[op](p, color_type::multiply(r, a), color_type::multiply(g, a), color_type::multiply(b, a), color_type::multiply(a, a), cover);"
R1=$?

# 2. The geometric cover is applied twice: get() scales by a cover that is
#    itself scaled by the cover.  One changed behavior, no source change.
run_mutation 2 "$NAME2" "agg_pixfmt_base.h" apply_line \
"double x = double(cover) / cover_full;" \
"double x = double(color_type::mult_cover(cover, cover)) / cover_full;"
R2=$?

# 3. The premultiplied blender drops the destination alpha contribution
#    (Da*(1 - Sa)) from the output alpha.
run_mutation 3 "$NAME3" "agg_pixfmt_rgba.h" apply_alpha_line \
"p[Order::A] = color_type::prelerp(p[Order::A], alpha, alpha);" \
"p[Order::A] = alpha;"
R3=$?

# 4. After left clipping, renderer_base forgets to advance the cover pointer,
#    so every destination pixel consumes the cover of the first source pixel.
run_mutation 4 "$NAME4" "agg_renderer_base.h" apply_line \
"covers += xmin() - x;" \
"/* mutation: clipped span does not advance cover start */"
R4=$?

echo
echo "probe: exit codes 0=detected 1=not detected 2=harness error"

for _r in "$R1" "$R2" "$R3" "$R4"; do
    if [ "$_r" -ne 0 ]; then FAILED=1; fi
done

if [ "$FAILED" -ne 0 ]; then
    echo
    echo "MUTATION CHECK FAILED: at least one injected fault was not detected by an assertion."
    exit 1
fi

echo
echo "MUTATION CHECK PASSED: all four single-point faults were detected by assertions."
exit 0
