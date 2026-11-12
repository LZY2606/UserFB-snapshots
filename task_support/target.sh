#!/bin/bash
#
# Fixed build/test/mutation wrapper for the libimagequant C-implementation
# regression suite.
#
#   bash task_support/target.sh                # four new groups + 8-pixel baseline
#   bash task_support/target.sh --mutations    # above, plus two source mutations
#
# The wrapper performs every build itself. A mutation counts as detected only
# when the *same* test cases fail through an executed Case::check content
# assertion on a successfully compiled binary. Compile errors, crashes
# (termination by signal), an empty registry and runs that execute no
# assertions are explicitly rejected as detection.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$PWD"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/liqtarget.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

TEST_GROUPS=(metric search layout reuse)
CSRC="libimagequant blur mediancut mempool nearest pam kmeans"
CFLAGS="-std=c99 -O1 -DUSE_SSE=0"
CXXFLAGS="-std=c++17 -O1"

# Build the C objects from a given source tree, then link the registry runner
# (task_support/main.cpp + task_tests/target.cpp) against them.
build_variant () { # $1 = label, $2 = source directory
    local label="$1"
    local srcdir="$2"
    local obj="$WORK/${label}_obj"
    mkdir -p "$obj"
    for f in $CSRC; do
        cc $CFLAGS -I"$srcdir" -c "$srcdir/$f.c" -o "$obj/$f.o"
    done
    c++ $CXXFLAGS -I"$srcdir" -Itask_support -Itask_tests \
        task_support/main.cpp task_tests/target.cpp "$obj"/*.o \
        -o "$WORK/${label}_runner" -lm
}

# Normal source: every group must complete and execute real assertions.
expect_pass () { # $1 = runner, $2 = group
    local out rc n
    set +e
    out="$("$1" "$2" 2>&1)"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        echo "$out" >&2
        echo "FATAL: group '$2' failed on unmodified source" >&2
        return 1
    fi
    echo "$out"
    echo "$out" | grep -q "GROUP $2 .* COMPLETE"
    n="$(echo "$out" | sed -n 's/.*assertions=\([0-9][0-9]*\).*/\1/p' | awk '{s+=$1} END{print s+0}')"
    if [ "$n" -le 0 ]; then
        echo "FATAL: group '$2' executed no assertions" >&2
        return 1
    fi
}

# Mutated source: the group must fail via a Case::check content assertion.
expect_detected () { # $1 = runner, $2 = group
    local out rc
    set +e
    out="$("$1" "$2" 2>&1)"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "$out" >&2
        echo "FATAL: mutation NOT detected by group '$2' (suite still passed)" >&2
        return 1
    fi
    if [ "$rc" -ge 128 ]; then
        echo "$out" >&2
        echo "FATAL: group '$2' terminated by signal (rc=$rc); a crash is not detection" >&2
        return 1
    fi
    if echo "$out" | grep -Eq "zero matching tests|zero assertions"; then
        echo "$out" >&2
        echo "FATAL: group '$2' failed without executing its assertions" >&2
        return 1
    fi
    if ! echo "$out" | grep -q "^FAIL $2:"; then
        echo "$out" >&2
        echo "FATAL: group '$2' did not fail through a content assertion (rc=$rc)" >&2
        return 1
    fi
    echo "  detected by $2: $(echo "$out" | grep "^FAIL $2:" | head -1)"
}

copy_sources () { # $1 = destination directory
    mkdir -p "$1"
    cp "$ROOT"/*.c "$ROOT"/*.h "$1"/
}

apply_mutation () { # $1 = mutated source directory, $2 = mutation name
    local srcdir="$1" name="$2"
    case "$name" in
        no_red_premultiply)
            # Drop premultiplication of the red channel in rgba_to_f (pam.h).
            grep -q '\.r = gamma_lut\[px\.r\]\*a,' "$srcdir/pam.h" \
                || { echo "FATAL: red-premultiply anchor not found in pam.h" >&2; exit 1; }
            perl -0pi -e 's/\.r = gamma_lut\[px\.r\]\*a,/.r = gamma_lut[px.r],/' "$srcdir/pam.h"
            grep -q '\.r = gamma_lut\[px\.r\],' "$srcdir/pam.h" \
                || { echo "FATAL: red-premultiply mutation did not apply" >&2; exit 1; }
            ;;
        no_vp_far_branch)
            # Remove one far-side search branch of the VP-tree (nearest.c).
            grep -q 'if (node->far && distance >= node->radius - best_candidate->distance) {' "$srcdir/nearest.c" \
                || { echo "FATAL: VP-tree far-branch anchor not found in nearest.c" >&2; exit 1; }
            perl -0pi -e 's/if \(node->far && distance >= node->radius - best_candidate->distance\) \{/if (0 \&\& node->far \&\& distance >= node->radius - best_candidate->distance) {/' "$srcdir/nearest.c"
            grep -q 'if (0 && node->far && distance >= node->radius - best_candidate->distance) {' "$srcdir/nearest.c" \
                || { echo "FATAL: VP-tree far-branch mutation did not apply" >&2; exit 1; }
            ;;
        *)
            echo "FATAL: unknown mutation '$name'" >&2; exit 1 ;;
    esac
}

echo "== Building unmodified library and test runner =="
build_variant normal "$ROOT"
NORMAL_RUNNER="$WORK/normal_runner"

echo "== Stage 1: four new regression groups (unmodified source) =="
for g in "${TEST_GROUPS[@]}"; do
    expect_pass "$NORMAL_RUNNER" "$g"
done

echo "== Stage 2: existing eight-pixel basic quantization baseline =="
bash task_support/baseline.sh

if [ "${1:-}" != "--mutations" ]; then
    echo "== All non-mutation stages completed successfully =="
    exit 0
fi

echo "== Stage 3: source mutations, each in an isolated temporary copy =="
for name in no_red_premultiply no_vp_far_branch; do
    echo "-- mutation: $name"
    srcdir="$WORK/mut_${name}"
    copy_sources "$srcdir"
    apply_mutation "$srcdir" "$name"
    # A successful build is required: a compile error must not count as detection.
    build_variant "mut_${name}" "$srcdir"
    runner="$WORK/mut_${name}_runner"
    for g in "${TEST_GROUPS[@]}"; do
        expect_detected "$runner" "$g"
    done
done

echo "== Both mutations were rejected by content assertions in every group =="
