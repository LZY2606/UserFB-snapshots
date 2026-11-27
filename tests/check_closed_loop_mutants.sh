#!/bin/bash
#
# tests/check_closed_loop_mutants.sh
#
# Mutation testing for tests/closed_loop.c.  Two separate, temporary copies
# of the whole source tree are created.  Each copy receives exactly one
# mutation:
#
#   mutant 1 (long_term.c): the strict cross-correlation maximum test
#             "if (L_result > L_max)" in the integer LTP parameter
#             calculation is relaxed to ">=", so equal positive
#             correlations no longer keep the first lag encountered.
#
#   mutant 2 (code.c):      the closed loop history feedback
#             GSM_ADD(S->e[5+i], dpp[i]) (quantized reconstructed
#             residual plus prediction) is replaced by the current
#             ORIGINAL short term residual so[k*40+i], feeding back
#             a value the encoder never actually reconstructed.
#
# A mutant only counts as detected when it (a) still compiles cleanly and
# (b) the fixed runner rejects the content with exit status 4 after the
# test prints CONTENT_MISMATCH.  A build failure or an abnormal
# termination (crash / any other status) is explicitly NOT a detection.
#
# The original source tree is never modified: every edit happens inside a
# private temporary copy that is removed on exit.
#
# Requirements: a POSIX shell, a C compiler and the standard utilities
# (cp, sed, grep, mv, rm).  Nothing is downloaded.

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
cc_bin="${CC:-cc}"
flags=(-std=c99 -O2 -Wno-deprecated-non-prototype -Wno-comment
       -I. -Iinc -DSASR -DWAV49 -DNeedFunctionPrototypes=1)
sources=(src/add.c src/code.c src/debug.c src/decode.c src/long_term.c
         src/lpc.c src/preprocess.c src/rpe.c src/gsm_destroy.c
         src/gsm_decode.c src/gsm_encode.c src/gsm_explode.c
         src/gsm_implode.c src/gsm_create.c src/gsm_print.c
         src/gsm_option.c src/short_term.c src/table.c)

work_root="$(mktemp -d "${TMPDIR:-/tmp}/gsm-closed-loop-mutants.XXXXXX")"
cleanup() { rm -rf "$work_root"; }
trap cleanup EXIT

# Guard against any accidental write to the real tree: all copies live
# under the freshly created temporary directory.
case "$work_root" in
    "$root"|"$root"/*)
        echo "ERROR: temporary directory would overlap the source tree" >&2
        exit 2
        ;;
esac

# build_and_run <copy-dir> <mutant-label>
#
# Returns 0 when the mutant is detected exactly as required (clean build,
# runner exits 4, CONTENT_MISMATCH present).  Returns non-zero otherwise.
build_and_run() {
    local copy="$1" label="$2"
    local log obj
    log="$copy/build.log"

    ( cd "$copy" || exit 90
      mkdir -p build
      : > "$log"
      for src in "${sources[@]}"; do
          obj="build/$(basename "${src%.c}").o"
          "$cc_bin" "${flags[@]}" -c "$src" -o "$obj" >>"$log" 2>&1 || exit 91
      done
      ar rcs build/library.a build/*.o >>"$log" 2>&1 || exit 92
      "$cc_bin" "${flags[@]}" task_support/driver.c tests/closed_loop.c \
          build/library.a -lm -o build/target >>"$log" 2>&1 || exit 93
    )
    local build_status=$?
    if [ "$build_status" -ne 0 ]; then
        echo "MUTANT $label: BUILD FAILED (not a valid detection)" >&2
        sed 's/^/    /' "$log" >&2 || true
        return 1
    fi

    "$copy/build/target" >"$copy/run.out" 2>&1
    local run_status=$?
    if [ "$run_status" -ne 4 ]; then
        echo "MUTANT $label: runner status $run_status, expected 4" >&2
        echo "  (abnormal termination or wrong status is not a detection)" >&2
        sed 's/^/    /' "$copy/run.out" >&2 || true
        return 1
    fi
    if ! grep -q "CONTENT_MISMATCH" "$copy/run.out"; then
        echo "MUTANT $label: exit 4 without CONTENT_MISMATCH output" >&2
        sed 's/^/    /' "$copy/run.out" >&2 || true
        return 1
    fi

    echo "MUTANT $label: DETECTED (build OK, runner exit 4, CONTENT_MISMATCH)"
    sed -n 's/^\(CONTENT_MISMATCH.*\)$/    \1/p' "$copy/run.out" | head -3
    return 0
}

# make_copy <destination>: clone the pieces needed to build and test.
make_copy() {
    local dst="$1"
    mkdir -p "$dst"
    cp -R "$root/src" "$root/inc" "$root/tests" "$root/task_support" "$dst/"
}

failures=0

# ---- Mutant 1: relaxed LTP correlation comparison ----------------------
copy1="$work_root/mutant1"
make_copy "$copy1"
# The active integer (non-cut, non-float) LTP loop is uniquely identified
# by the fully expanded STEP(39) accumulation directly above its strict
# maximum test.  Replace only that comparison; the cut/float variants are
# left untouched.  awk is used (not GNU sed brace syntax) for portability.
awk '/L_result \+= STEP\(39\) ;/ { armed = 1 }
     armed && /if \(L_result > L_max\)/ {
         sub(/if \(L_result > L_max\)/, "if (L_result >= L_max)")
         armed = 0
     }
     { print }' \
    "$root/src/long_term.c" > "$copy1/src/long_term.c.tmp"
mv "$copy1/src/long_term.c.tmp" "$copy1/src/long_term.c"
if ! grep -q "if (L_result >= L_max)" "$copy1/src/long_term.c"; then
    echo "MUTANT 1: failed to apply edit" >&2
    exit 2
fi
if grep -q "if (L_result >= L_max)" "$root/src/long_term.c"; then
    echo "MUTANT 1: original source was modified -- aborting" >&2
    exit 2
fi
build_and_run "$copy1" "1-ltp-non-strict-max" || failures=$((failures + 1))

# ---- Mutant 2: closed loop feeds back the original residual ------------
copy2="$work_root/mutant2"
make_copy "$copy2"
sed 's#dp\[ i \] = GSM_ADD( S->e\[5 + i\], dpp\[i\] );#dp[ i ] = GSM_ADD( so[k*40 + i], dpp[i] );#' \
    "$root/src/code.c" > "$copy2/src/code.c.tmp"
mv "$copy2/src/code.c.tmp" "$copy2/src/code.c"
if ! grep -q "GSM_ADD( so\[k\*40 + i\], dpp\[i\] )" "$copy2/src/code.c"; then
    echo "MUTANT 2: failed to apply edit" >&2
    exit 2
fi
if grep -q "so\[k\*40 + i\]" "$root/src/code.c"; then
    echo "MUTANT 2: original source was modified -- aborting" >&2
    exit 2
fi
build_and_run "$copy2" "2-code-original-residual-feedback" || failures=$((failures + 1))

if [ "$failures" -ne 0 ]; then
    echo "CLOSED_LOOP_MUTANTS: $failures mutant(s) not properly detected" >&2
    exit 1
fi
echo "CLOSED_LOOP_MUTANTS: both mutants detected via content assertions"
exit 0
