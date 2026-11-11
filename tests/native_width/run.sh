#!/bin/bash
#
# Native width/flag/effective-address regression tests for bddisasm + bdshemu.
#
# 1. Builds the unmodified library and runs native_width_test, which feeds the
#    same hand-assembled bytes to NdDecodeEx and ShemuEmulate and prints one
#    named PASS line per scenario.
# 2. For each of the three independent benign degradation patches, copies the
#    library sources to a fresh temporary tree, applies the patch, rebuilds and
#    runs the same binary. A mutation counts as detected only when:
#       - the patch applies cleanly,
#       - the mutated tree compiles cleanly,
#       - the binary exits normally with status 1 (never by signal),
#       - and its output contains the expected named BEHAVIOR_FAIL marker.
#    Anything else (apply failure, compile error, signal exit) aborts the whole
#    script instead of being counted. The working tree sources are never
#    modified: every mutation lives in a temporary copy that is removed on exit.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
cd "$repo_root"

cc_bin="${CC:-cc}"
base_flags=(-std=c11 -O0 -Wno-unknown-pragmas
            -DBDDISASM_HAS_VSNPRINTF -DBDDISASM_HAS_MEMSET)
test_src="tests/native_width/native_width_test.c"

# Scratch root: $TMPDIR by default; NATIVE_WIDTH_TMP may override it for
# environments where the default temp area is not writable.
tmp_root="${NATIVE_WIDTH_TMP:-${TMPDIR:-/tmp}}"
work="$(mktemp -d "$tmp_root/native_width.XXXXXX")"
trap 'rm -rf "$work"' EXIT

echo "=== Normal (unmodified) build ==="
normal_out="$work/normal.bin"
normal_log="$work/normal.log"
# The normal tree is the repository itself; compile straight from it.
"$cc_bin" "${base_flags[@]}" -Iinc -Ibddisasm/include \
    bddisasm/*.c bdshemu/*.c "$test_src" -o "$normal_out"
set +e
"$normal_out" | tee "$normal_log"
normal_rc=${PIPESTATUS[0]}
set -e
if [ "$normal_rc" -ne 0 ]; then
    echo "FATAL: normal build test failed (rc=$normal_rc)" >&2
    exit 1
fi
pass_count="$(grep -c '^PASS ' "$normal_log")"
if [ "$pass_count" -ne 9 ]; then
    echo "FATAL: expected 9 named PASS lines, got $pass_count" >&2
    exit 1
fi
grep -q '^SCENARIOS_EXECUTED 9$' "$normal_log"
grep -q '^BEHAVIOR_ASSERTIONS 3 ' "$normal_log"
grep -q '^NATIVE_WIDTH_OK$' "$normal_log"

# patch name | expected named behavior marker
mutations=(
    "mut_32bit_zero_extend|BEH_32BIT_ZERO_EXTEND"
    "mut_high8_selection|BEH_HIGH8_SELECTION"
    "mut_addrsize_override|BEH_ADDRSIZE_OVERRIDE"
)

echo
echo "=== Degradation patch detection ==="
detected=0
for entry in "${mutations[@]}"; do
    patch_name="${entry%%|*}"
    marker="${entry##*|}"
    patch_file="tests/native_width/patches/$patch_name.patch"
    case_dir="$work/$patch_name"
    mkdir -p "$case_dir"
    cp -R bddisasm bdshemu inc "$case_dir/"

    if ! patch -d "$case_dir" -p1 --forward --batch < "$patch_file" >"$case_dir/apply.log" 2>&1; then
        echo "FATAL: patch $patch_name failed to apply" >&2
        cat "$case_dir/apply.log" >&2
        exit 1
    fi
    echo "MUTATION $patch_name: apply=ok"

    mut_bin="$case_dir/mut.bin"
    mut_log="$case_dir/mut.log"
    if ! "$cc_bin" "${base_flags[@]}" \
           -I"$case_dir/inc" -I"$case_dir/bddisasm/include" \
           "$case_dir"/bddisasm/*.c "$case_dir"/bdshemu/*.c "$test_src" -o "$mut_bin" \
           >"$case_dir/cc.log" 2>&1; then
        echo "FATAL: patch $patch_name broke compilation" >&2
        cat "$case_dir/cc.log" >&2
        exit 1
    fi
    echo "MUTATION $patch_name: compile=ok"

    set +e
    "$mut_bin" > "$mut_log" 2>&1
    mut_rc=$?
    set -e
    if [ "$mut_rc" -ge 128 ]; then
        echo "FATAL: patch $patch_name killed the test by signal (rc=$mut_rc)" >&2
        exit 1
    fi
    if [ "$mut_rc" -ne 1 ]; then
        echo "FATAL: patch $patch_name did not fail the test (rc=$mut_rc); expected 1" >&2
        exit 1
    fi
    if ! grep -q "BEHAVIOR_FAIL $marker:" "$mut_log"; then
        echo "FATAL: patch $patch_name failed, but not via named assertion $marker" >&2
        cat "$mut_log" >&2
        exit 1
    fi
    # The set of distinct named behavior markers tripped must be exactly this
    # patch's own marker; collateral damage to another behavior assertion is
    # treated as an invalid mutation.
    markers_failed="$(grep 'BEHAVIOR_FAIL ' "$mut_log" \
                      | sed -E 's/.*BEHAVIOR_FAIL ([A-Z0-9_]+):.*/\1/' \
                      | sort -u | tr '\n' ' ')"
    if [ "$markers_failed" != "$marker " ]; then
        echo "FATAL: patch $patch_name tripped markers [$markers_failed], expected only [$marker]" >&2
        cat "$mut_log" >&2
        exit 1
    fi
    detected=$((detected + 1))
    echo "MUTATION $patch_name: DETECTED via $marker"
done

if [ "$detected" -ne 3 ]; then
    echo "FATAL: expected 3 detected mutations, got $detected" >&2
    exit 1
fi

echo
echo "SCENARIOS_EXECUTED $pass_count"
echo "BEHAVIOR_ASSERTIONS_DETECTED 3 BEH_32BIT_ZERO_EXTEND BEH_HIGH8_SELECTION BEH_ADDRSIZE_OVERRIDE"
echo "NATIVE_WIDTH_OK"
