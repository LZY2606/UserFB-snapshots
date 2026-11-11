#!/usr/bin/env bash
set -euo pipefail

tmp="${TMPDIR:-/tmp/claude}/bvh-reinsert-contract.$$"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

cxx_flags=(-std=c++20 -O2 -pthread)
contract_src=devcheck/reinsert_contract.cpp

sed_inplace() {
    local expr="$1"
    local file="$2"
    if sed --version >/dev/null 2>&1; then
        sed -i -E -e "$expr" "$file"
    else
        sed -i '' -E -e "$expr" "$file"
    fi
}

# Normal source must pass every contract phase and both upstream tests.
c++ "${cxx_flags[@]}" -I src "$contract_src" -o "$tmp/contract"
if ! "$tmp/contract" "$tmp/normal.ppm" > "$tmp/normal.log" 2>&1; then
    echo "FAIL normal source copy contract"
    cat "$tmp/normal.log"
    exit 1
fi
cat "$tmp/normal.log"
grep -q '^PASS optimized-topology$' "$tmp/normal.log"
grep -q '^PASS subtree-identity$' "$tmp/normal.log"
grep -q '^PASS ray-reference$' "$tmp/normal.log"
grep -q '^PASS pixels$' "$tmp/normal.log"

c++ "${cxx_flags[@]}" -I src test/simple_example.cpp -o "$tmp/simple"
"$tmp/simple"

c++ "${cxx_flags[@]}" -I src test/serialize.cpp -o "$tmp/serialize"
(
    cd "$tmp"
    ./serialize
)

# Patch the refit merge so internal bboxes only contain the left child.
patch_refit_child() {
    sed_inplace \
        's/node\.set_bbox\(left\.get_bbox\(\)\.extend\(right\.get_bbox\(\)\)\);/node.set_bbox(left.get_bbox());/' \
        "$1"
}

# Patch extracted leaves so they copy primitive ids from the start of the
# original array instead of from their own source leaf interval.
patch_subtree_offset() {
    sed_inplace \
        's/prim_ids\.begin\(\) \+ src_node\.index\.first_id\(\),/prim_ids.begin(),/' \
        "$1"
}

# Patch reinsertion so the target subtree's ancestors are not refitted.
patch_reinsert_bounds() {
    sed_inplace '/^        refit_from\(to\);$/d' "$1"
}

run_mutant() {
    local name="$1"
    local expected_tag="$2"
    local patch_fn="$3"
    local file="$4"

    cp -R src "$tmp/$name"
    "$patch_fn" "$tmp/$name/bvh/v2/$file"

    if ! c++ "${cxx_flags[@]}" -I "$tmp/$name" "$contract_src" -o "$tmp/$name.bin"; then
        echo "FAIL $name: source copy did not compile"
        exit 1
    fi

    set +e
    "$tmp/$name.bin" "$tmp/$name.ppm" > "$tmp/$name.log" 2>&1
    local rc=$?
    set -e

    if [ "$rc" -ne 1 ]; then
        echo "FAIL $name: expected assertion exit 1, got $rc"
        cat "$tmp/$name.log"
        exit 1
    fi
    if ! grep -q "DETECTED $expected_tag" "$tmp/$name.log"; then
        echo "FAIL $name: expected DETECTED $expected_tag"
        cat "$tmp/$name.log"
        exit 1
    fi
    cat "$tmp/$name.log"
}

run_mutant refit-child refit-child patch_refit_child bvh.h
run_mutant subtree-offset subtree-offset patch_subtree_offset bvh.h
run_mutant reinsert-bounds reinsert-bounds patch_reinsert_bounds reinsertion_optimizer.h

echo BVH_REINSERT_CONTRACT_OK
