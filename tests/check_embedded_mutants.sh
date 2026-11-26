#!/bin/bash
# tests/check_embedded_mutants.sh - mutation checks for the embedded/QMF
# contract tests.
#
# Each mutant is staged in an isolated copy of the sources, must build
# cleanly, and must then fail the same content tests with a numeric
# difference (driver exit code 4).  Build errors, crashes and missing
# files never count as a detection.  An unmodified control copy must pass.
set -u

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
work=$(mktemp -d "${TMPDIR:-/tmp}/g722-mutants.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM
status=0

copy_tree() {
    dst=$1
    mkdir -p "$dst/task_support" "$dst/tests" || return 1
    for f in g722_encode.c g722_decode.c g722_common.h g722_private.h \
             g722_encoder.h g722_decoder.h g722.h; do
        if [ ! -f "$repo/$f" ]; then
            echo "ERROR: missing source file $f"
            return 1
        fi
        cp "$repo/$f" "$dst/$f" || return 1
    done
    cp "$repo/task_support/driver.c" "$repo/task_support/check.h" "$dst/task_support/" || return 1
    cp "$repo/tests/embedded_qmf_contract.c" "$dst/tests/" || return 1
}

# build_and_run <dir> <ok|fail> <label>
build_and_run() {
    dir=$1
    expect=$2
    label=$3
    if ! (cd "$dir" && "$cc" -std=c99 -O2 -I. \
            task_support/driver.c tests/embedded_qmf_contract.c \
            g722_encode.c g722_decode.c -lm -o target) >"$dir/build.log" 2>&1; then
        echo "NOT-DETECTED $label: build failed (a build error is not a detection)"
        sed 's/^/  /' "$dir/build.log" | head -20
        return 1
    fi
    (cd "$dir" && ./target) >"$dir/run.log" 2>&1
    st=$?
    if [ "$expect" = ok ]; then
        if [ $st -eq 0 ]; then
            echo "OK $label: unmodified sources pass the contract tests"
            return 0
        fi
        echo "CONTROL-FAIL $label: unmodified sources exited $st"
        sed 's/^/  /' "$dir/run.log" | head -20
        return 1
    fi
    if [ $st -eq 4 ]; then
        echo "DETECTED $label: content tests failed on the mutant (exit 4)"
        grep -m 2 -E '^(FAIL|MISMATCH|STATE)' "$dir/run.log" | sed 's/^/  /'
        return 0
    fi
    if [ $st -eq 0 ]; then
        echo "NOT-DETECTED $label: contract tests passed on the mutant"
        return 1
    fi
    echo "NOT-DETECTED $label: exit $st (a crash or structural error is not a detection)"
    sed 's/^/  /' "$dir/run.log" | head -20
    return 1
}

# Mutant 1: transmit QMF - reverse the direction of one coefficient lane.
mutate_qmf_reverse() {
    src=$1/g722_encode.c
    grep -qF 'sumodd += s->x[2*i]*qmf_coeffs[i];' "$src" || return 1
    sed 's|sumodd += s->x\[2\*i\]\*qmf_coeffs\[i\];|sumodd += s->x[2*i]*qmf_coeffs[11 - i];|' \
        "$src" > "$src.tmp" || return 1
    grep -qF 'sumodd += s->x[2*i]*qmf_coeffs[11 - i];' "$src.tmp" || { rm -f "$src.tmp"; return 1; }
    mv "$src.tmp" "$src"
}

# Mutant 2: 56kbps decode uses the 6-bit reconstruction table.
mutate_recon_table() {
    src=$1/g722_decode.c
    grep -qF 'wd2 = qm5[wd1];' "$src" || return 1
    sed 's|wd2 = qm5\[wd1\];|wd2 = qm6[wd1];|' "$src" > "$src.tmp" || return 1
    grep -qF 'wd2 = qm6[wd1];' "$src.tmp" || { rm -f "$src.tmp"; return 1; }
    mv "$src.tmp" "$src"
}

# Mutant 3: packed encode clears the residual bits at the start of every call.
mutate_packed_flush() {
    src=$1/g722_encode.c
    grep -qF '    g722_bytes = 0;' "$src" || return 1
    awk '{ print } $0 == "    g722_bytes = 0;" { print "    s->out_buffer = 0;"; print "    s->out_bits = 0;" }' \
        "$src" > "$src.tmp" || return 1
    grep -qF '    s->out_bits = 0;' "$src.tmp" || { rm -f "$src.tmp"; return 1; }
    mv "$src.tmp" "$src"
}

run_one() {
    name=$1
    mutate=$2
    dir="$work/$name"
    if ! copy_tree "$dir"; then
        echo "ERROR $name: could not stage the isolated copy"
        return 1
    fi
    if ! "$mutate" "$dir"; then
        echo "ERROR $name: mutation did not apply (pattern not found)"
        return 1
    fi
    build_and_run "$dir" fail "$name"
}

control="$work/control"
if ! copy_tree "$control"; then
    echo "ERROR control: could not stage the isolated copy"
    exit 1
fi
build_and_run "$control" ok control || status=1

run_one qmf_reverse mutate_qmf_reverse || status=1
run_one recon_table mutate_recon_table || status=1
run_one packed_flush mutate_packed_flush || status=1

if [ $status -eq 0 ]; then
    echo "ALL_MUTANTS_DETECTED"
else
    echo "MUTANT_CHECK_FAILED"
fi
exit $status
