#!/usr/bin/env bash
#
# Runner for the cross-samplerate LC3 MDCT numerical contract.
#
#   1. Builds and runs checks/mdct_contract.c + checks/mdct_reference.c
#      against the unmodified library (20 rate/duration/in-place cases
#      plus the public encoder/decoder ABI chain).
#   2. Repeats the run in three independent temporary copies of the
#      sources, each carrying one injected defect:
#        M1  forward cross-rate gain omitted
#        M2  inverse cross-rate gain inverted
#        M3  synthesis window drops the previous-frame overlap history
#      A copy only counts as "caught" when it compiles cleanly and the
#      test binary fails through a numerical assertion
#      (MDCT_CONTRACT_FAIL), never through a compile error, a missing
#      file, a crash or a timeout.
#
#   The original tree is never modified.
#
set -euo pipefail

cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Iinclude -Isrc"

b=$(mktemp -d 2>/dev/null) \
    || b=$(mktemp -d "${TMPDIR:-/tmp}/mdct-contract.XXXXXX" 2>/dev/null) \
    || b=$(mktemp -d ./.mdct-contract.XXXXXX)
trap 'rm -rf "$b"' EXIT

# Portable in-place sed (GNU vs BSD)
if sed --version >/dev/null 2>&1; then SED_I=(-i); else SED_I=(-i ''); fi

# run_with_timeout <seconds> <cmd...>; returns 124 on timeout
run_with_timeout() {
    local secs=$1; shift
    "$@" &
    local pid=$!
    ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) &
    local watchdog=$!
    local rc=0
    wait "$pid" 2>/dev/null || rc=$?
    kill "$watchdog" 2>/dev/null || true
    wait "$watchdog" 2>/dev/null || true
    if [ "$rc" -gt 128 ]; then rc=124; fi
    return "$rc"
}

echo "== reference build (unmodified tree) =="
$CC $CFLAGS checks/mdct_contract.c checks/mdct_reference.c src/*.c \
    -lm -o "$b/contract"

if ! run_with_timeout 300 "$b/contract" > "$b/ref.out" 2>&1; then
    echo "FATAL: reference contract run failed on the unmodified tree" >&2
    cat "$b/ref.out" >&2
    exit 1
fi
cat "$b/ref.out"

cases=$(sed -n 's/^MDCT_CONTRACT_CASES cases=\([0-9][0-9]*\)$/\1/p' "$b/ref.out")
if [ -z "$cases" ] || [ "$cases" -lt 20 ]; then
    echo "FATAL: expected at least 20 executed cases, got '${cases:-none}'" >&2
    exit 1
fi
if grep -qi 'skip' "$b/ref.out"; then
    echo "FATAL: skipped cases are not allowed" >&2
    exit 1
fi

echo "== mutant copies =="

mutant_names=(
    "M1 forward cross-rate gain omitted"
    "M2 inverse cross-rate gain inverted"
    "M3 synthesis window drops previous-frame overlap history"
)
mutant_seds=(
    's|sqrtf((float)ns_dst / ns)|1.0f|'
    's|sqrtf((float)ns / ns_src)|sqrtf((float)ns_src / ns)|'
    's|\*(--x0)|0.0f|g; s|\*(x1++)|0.0f|g'
)

caught=0
for m in 0 1 2; do
    dir="$b/mutant$m"
    mkdir -p "$dir"
    cp -R include src checks "$dir/"

    # Inject the defect into the copy only; verify it actually applied.
    sed "${SED_I[@]}" "${mutant_seds[$m]}" "$dir/src/mdct.c"
    if cmp -s src/mdct.c "$dir/src/mdct.c"; then
        echo "FATAL: mutation $m did not apply to the copy" >&2
        exit 1
    fi

    # The copy must build on its own; a compile error is a harness
    # failure, never a detected defect.
    if ! ( cd "$dir" && $CC $CFLAGS \
            checks/mdct_contract.c checks/mdct_reference.c src/*.c \
            -lm -o contract ) 2> "$dir/build.log"; then
        echo "FATAL: mutant copy $m failed to compile" >&2
        cat "$dir/build.log" >&2
        exit 1
    fi

    rc=0
    run_with_timeout 300 "$dir/contract" > "$dir/run.out" 2>&1 || rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "FATAL: mutant $m was NOT detected (clean exit)" >&2
        cat "$dir/run.out" >&2
        exit 1
    fi
    if [ "$rc" -eq 124 ]; then
        echo "FATAL: mutant $m run timed out (not a numerical detection)" >&2
        exit 1
    fi
    if ! grep -q '^MDCT_CONTRACT_FAIL' "$dir/run.out"; then
        echo "FATAL: mutant $m failed without a numerical assertion" >&2
        cat "$dir/run.out" >&2
        exit 1
    fi

    first_dev=$(grep -m1 '^MDCT_CONTRACT_FAIL' "$dir/run.out")
    echo "mutant[$m] ${mutant_names[$m]}"
    echo "  first numerical deviation: $first_dev"
    caught=$((caught + 1))
done

if [ "$caught" -ne 3 ]; then
    echo "FATAL: only $caught/3 mutants caught" >&2
    exit 1
fi

echo "LC3_MDCT_CONTRACT_OK cases=$cases mutants=$caught"
