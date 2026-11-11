#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/tiny_bvh_motion_instance_test.cpp || { echo 'missing motion instance regression source' >&2; exit 1; }
mkdir -p build-check
for mode in scalar native; do
    flags=()
    if [[ "$mode" == scalar ]]; then flags+=(-DTINYBVH_NO_SIMD); else flags+=(-march=native); fi
    "${CXX:-c++}" -std=c++20 -O2 -pthread -I. "${flags[@]}" tests/tiny_bvh_motion_instance_test.cpp -o "build-check/motion-$mode"
    "build-check/motion-$mode" > "build-check/motion-$mode.stdout"
    cat "build-check/motion-$mode.stdout"
    for group in affine swept shadow identity bundle static; do
        grep -Fx "MOTION $group PASS" "build-check/motion-$mode.stdout" >/dev/null || { echo "missing executed group: $mode/$group" >&2; exit 1; }
    done
    echo "MOTION_MODE $mode PASS"
done
