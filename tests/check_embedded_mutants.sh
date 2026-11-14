#!/bin/bash
# check_embedded_mutants.sh - mutation-detection harness for the C contract.
#
# Each mutant is built in an isolated copy of the tree.  A mutant counts as
# DETECTED only when:
#   1. the unmutated baseline of the same copy builds and exits 0 (so the
#      harness itself is sound and the file set is complete), and
#   2. the mutated copy still compiles, and
#   3. the mutated build runs to completion and fails on a content assertion,
#      i.e. the driver reports a numeric FAIL and exits with its "case failed"
#      status.
#
# A build error, a crash/signal, a missing file or an untouched passing run is
# never counted as detection.  The three degradations exercised are:
#   - analysis: one transmit QMF branch reads its coefficients backwards;
#   - embedded: the 56 kbit/s decoder uses the six-bit reconstruction table;
#   - packing:  the packed encoder drops its carried residual at each call start.
#
# Only C, shell and the system math library are needed; nothing is downloaded.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
work=$(mktemp -d "${TMPDIR:-/tmp}/g722-mutants.XXXXXX") || {
    echo "cannot create a scratch directory" >&2
    exit 1
}
trap 'rm -rf "$work"' EXIT HUP INT TERM

# driver exit status for a failed content case (see task_support/driver.c)
case_failed_status=4

pass=0
fail=0

copy_tree()
{
    dir=$1
    mkdir -p "$dir/task_support" "$dir/tests"
    cp "$root"/g722_encode.c "$root"/g722_decode.c \
       "$root"/g722_private.h "$root"/g722_common.h \
       "$root"/g722.h "$root"/g722_encoder.h "$root"/g722_decoder.h "$dir"/
    cp "$root"/task_support/check.h "$root"/task_support/driver.c "$dir/task_support"/
    cp "$root"/tests/embedded_qmf_contract.c "$dir/tests"/
}

build_tree()
{
    dir=$1
    bin=$2
    ( cd "$dir" && $cc -std=c99 -O2 -I. task_support/driver.c \
        tests/embedded_qmf_contract.c g722_encode.c g722_decode.c \
        -lm -o "$bin" ) >"$dir/build.log" 2>&1
}

run_tree()
{
    dir=$1
    bin=$2
    log=$dir/$3
    ( cd "$dir" && "./$bin" ) >"$log" 2>&1
    status=$?
    return $status
}

report_fail()
{
    name=$1
    reason=$2
    printf 'MUTANT %-12s NOT DETECTED: %s\n' "$name" "$reason"
    fail=$((fail + 1))
}

# Literal (non-regex) search/replace over the whole file, including multi-line
# searches.  Exactly one occurrence is required; anything else is an error.
apply_mutation()
{
    path=$1
    search=$2
    replace=$3

    if [ ! -f "$path" ]; then
        return 1
    fi
    if MUT_SEARCH=$search MUT_REPLACE=$replace awk '
        BEGIN { RS = "\001"; ORS = "" }
        NR == 1 {
            whole = $0
            slen = length(ENVIRON["MUT_SEARCH"])
            if (slen == 0) exit 2
            count = 0
            out = ""
            while ((pos = index(whole, ENVIRON["MUT_SEARCH"])) > 0 && count < 2) {
                out = out substr(whole, 1, pos - 1) ENVIRON["MUT_REPLACE"]
                whole = substr(whole, pos + slen)
                count++
            }
            if (count != 1) exit 2
            out = out whole
            printf "%s", out
        }
    ' "$path" > "$path.new"; then
        mv "$path.new" "$path"
        return 0
    fi
    rm -f "$path.new"
    return 1
}

# Usage: check_mutant <name> <relative-source-file> <search> <replace>
check_mutant()
{
    name=$1
    src=$2
    search=$3
    replace=$4

    dir="$work/$name"
    copy_tree "$dir"

    # 1. The pristine copy must build and pass.  This validates the harness,
    #    the file set and the content tests before any mutation is applied.
    if ! build_tree "$dir" baseline_bin; then
        report_fail "$name" "pristine copy failed to build; build errors are not detections"
        return
    fi
    if ! run_tree "$dir" baseline_bin baseline.log; then
        report_fail "$name" "pristine copy did not pass its own baseline"
        return
    fi
    if ! grep -q '^TARGET_COMPLETE ' "$dir/baseline.log"; then
        report_fail "$name" "pristine baseline produced no completion marker"
        return
    fi

    # 2. Apply the degradation with a literal search/replace (POSIX awk, no
    #    regex metacharacter surprises) and require exactly one match.
    if ! apply_mutation "$dir/$src" "$search" "$replace"; then
        report_fail "$name" "mutation target missing or not unique in $src"
        return
    fi

    # 3. The mutated copy must still compile; a build error is not a detection.
    if ! build_tree "$dir" mutant_bin; then
        report_fail "$name" "mutated copy failed to build; build errors are not detections"
        return
    fi

    run_tree "$dir" mutant_bin mutant.log
    status=$?

    if [ "$status" -ge 128 ]; then
        report_fail "$name" "crashed with signal $((status - 128)); crashes are not detections"
        return
    fi
    if grep -q 'FAIL ' "$dir/mutant.log" && [ "$status" -eq "$case_failed_status" ]; then
        first=$(grep -m1 'FAIL ' "$dir/mutant.log")
        detail=$(grep -m1 '  ' "$dir/mutant.log" | sed 's/^  //')
        if [ -n "$detail" ]; then
            printf 'MUTANT %-12s DETECTED: %s (%s)\n' "$name" "$first" "$detail"
        else
            printf 'MUTANT %-12s DETECTED: %s\n' "$name" "$first"
        fi
        pass=$((pass + 1))
        return
    fi
    if [ "$status" -eq 0 ]; then
        report_fail "$name" "all checks still passed; the degradation is not covered"
        return
    fi
    report_fail "$name" "unexpected exit $status without a numeric FAIL"
}

check_mutant qmf_reverse g722_encode.c \
    'sumeven += s->x[2*i + 1]*qmf_coeffs[11 - i];' \
    'sumeven += s->x[2*i + 1]*qmf_coeffs[i];'

check_mutant qm56_table g722_decode.c \
    'wd2 = qm5[wd1];' \
    'wd2 = qm6[wd1];'

check_mutant pack_reset g722_encode.c \
    '    g722_bytes = 0;
    xhigh = 0;' \
    '    g722_bytes = 0;
    if (s->packed)
    {
        s->out_buffer = 0;
        s->out_bits = 0;
    }
    xhigh = 0;'

echo "EMBEDDED_MUTANTS detected=$pass not_detected=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -eq 3 ]
