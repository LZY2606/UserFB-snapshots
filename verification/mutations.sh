#!/bin/bash
# Mutation testing for the reversible 5/3 DWT regression contract.
#
# Three independent copies of the source tree are built in temporary
# directories, each with a single deliberate production-code mutation:
#
#   origin-parity     forward horizontal transform ignores the origin parity
#   inverse-rounding  inverse update step drops the +2 rounding correction
#   singleton         odd-origin singleton forward transform skips the x2
#
# A mutation counts as detected only when the tree builds, the contract
# binary finishes within the time limit, executes real cases and reports a
# numeric DWT_FAIL mismatch. Build errors, crashes (signals), timeouts and
# zero executed cases never qualify. The working tree is never modified.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/dwt-mutation.XXXXXX")
trap 'rm -rf "$work"' EXIT

run_mutation() {
    name=$1
    apply=$2
    base="$work/$name"
    log="$base.log"
    rc=0
    {
        echo "== mutation $name =="
        cp -R "$root" "$base"
        if ! (cd "$base" && eval "$apply"); then
            echo "MUTATION_BUILD_ERROR $name (source edit)"
            rc=1

        else
            build="$base/mut-build"
            if ! cmake -S "$base" -B "$build" \
                    -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF \
                    -DBUILD_SHARED_LIBS=OFF -DBUILD_JPIP=OFF \
                    -DCMAKE_BUILD_TYPE=Release >"$base.configure.log" 2>&1; then
                echo "MUTATION_BUILD_ERROR $name (cmake configure)"
                rc=1
            elif ! cmake --build "$build" --target openjp2 -j4 \
                    >"$base.build.log" 2>&1; then
                echo "MUTATION_BUILD_ERROR $name (compile)"
                rc=1
            else
                cc -std=c99 -O2 -DOPJ_STATIC -I"$base" \
                   -I"$base/src/lib/openjp2" -I"$build/src/lib/openjp2" \
                   "$base/tests/dwt_contract.c" "$base/tests/dwt_reference.c" \
                   "$base/verification/driver_main.c" \
                   "$build/bin/libopenjp2.a" -lpthread -lm -o "$base/contract"
                if [ ! -x "$base/contract" ]; then
                    echo "MUTATION_BUILD_ERROR $name (contract link)"
                    rc=1
                else
                    # 60 seconds is far above the sub-second runtime of an
                    # unmutated run, so a timeout means the mutation broke
                    # control flow rather than a numeric comparison.
                    timeout 60 "$base/contract" >"$base.run.log" 2>&1
                    trc=$?
                    if [ "$trc" -eq 124 ]; then
                        echo "MUTATION_NOT_DETECTED $name (timeout)"
                        rc=1
                    elif [ "$trc" -ge 128 ]; then
                        echo "MUTATION_NOT_DETECTED $name (signal crash rc=$trc)"
                        rc=1
                    elif [ "$trc" -eq 0 ]; then
                        echo "MUTATION_NOT_DETECTED $name (contract unexpectedly passed)"
                        rc=1
                    else
                        case_marks=$(grep -cE '^CASE_(PASS|FAIL) ' "$base.run.log" || true)
                        fails=$(grep -c '^DWT_FAIL ' "$base.run.log" || true)
                        # A real detection needs the harness to have run at
                        # least one case and our comparison code to have
                        # reported a numeric mismatch. A failure in setup
                        # before any comparison leaves fails == 0 and does
                        # not count.
                        if [ "$case_marks" -lt 1 ] || [ "$fails" -lt 1 ]; then
                            echo "MUTATION_NOT_DETECTED $name (no executed numeric case: case_marks=$case_marks fails=$fails)"
                            rc=1
                        else
                            echo "first deviation:"
                            grep -m1 '^DWT_FAIL ' "$base.run.log"
                            echo "MUTATION_NUMERIC_PASS $name"
                        fi
                    fi
                fi
            fi
        fi
    } >"$log" 2>&1
    cat "$log"
    return "$rc"
}

# All anchors below live in src/lib/openjp2/dwt.c. They were chosen on the
# code paths actually compiled and executed on this target (scalar horizontal
# lifting and the scalar vertical fallback used by narrow column tails).

# 1. Force the even-origin formula regardless of the real horizontal parity.
apply_origin_parity='perl -0pi -e "s/const OPJ_INT32 sn = \(OPJ_INT32\)\(\(width \+ \(even \? 1 : 0\)\) >> 1\);/const OPJ_INT32 sn = (OPJ_INT32)((width + 1) >> 1); even = OPJ_TRUE;/" src/lib/openjp2/dwt.c'

# 2. Drop the +2 rounding correction of the inverse horizontal update steps
#    (both even- and odd-origin scalar lifting functions).
apply_inverse_rounding='perl -0pi -e "s/s1n - \(\(d1c \+ d1n \+ 2\) >> 2\)/s1n - ((d1c + d1n) >> 2)/g; s/in_odd\[0\] - \(\(in_even\[0\] \+ s1 \+ 2\) >> 2\)/in_odd[0] - ((in_even[0] + s1) >> 2)/g; s/in_odd\[j\] - \(\(s1 \+ s2 \+ 2\) >> 2\)/in_odd[j] - ((s1 + s2) >> 2)/g" src/lib/openjp2/dwt.c'

# 3. Odd-origin singleton forward transform must scale by two; remove it.
apply_singleton='perl -0pi -e "s/row\[0\] \*= 2;/row[0] *= 1;/" src/lib/openjp2/dwt.c'

status=0
run_mutation origin-parity "$apply_origin_parity" || status=1
run_mutation inverse-rounding "$apply_inverse_rounding" || status=1
run_mutation singleton "$apply_singleton" || status=1

if [ "$status" -eq 0 ]; then
    echo MUTATIONS_ALL_DETECTED
fi
exit "$status"
