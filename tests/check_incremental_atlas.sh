#!/bin/sh
#
# Headless regression suite for the incremental atlas interface.
#
# Run from the repository root:
#
#   sh tests/check_incremental_atlas.sh
#
# Needs only a C++17 compiler - no network, no display server.
# Any mismatch makes the script exit non-zero.

set -eu

cd "$(dirname "$0")/.."

work=$(mktemp -d "${TMPDIR:-/tmp}/rectpack2d-atlas.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

CXX=${CXX:-c++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -pedantic"

echo "== 1/5 empty_spaces baseline (incl. static provider rollback) =="
"$CXX" $FLAGS -Isrc tests/atlas/baseline.cpp -o "$work/baseline"
"$work/baseline" < tests/atlas/baseline.in > "$work/baseline.out"
cat "$work/baseline.out"
diff -u tests/atlas/baseline.expected "$work/baseline.out"

echo "== 2/5 incremental atlas unit tests =="
# Covers: no-rotation inserts, padding 0, squares, exact fills,
# partial-insert failure followed by retry, duplicate ids (old and in-batch),
# illegal dimensions, static provider exhaustion and instance isolation.
"$CXX" $FLAGS -Isrc tests/atlas/incremental_atlas_test.cpp -o "$work/unit"
"$work/unit"

echo "== 3/5 grayscale example vs hand-written fixtures =="
"$CXX" $FLAGS -Isrc example/grayscale_atlas.cpp -o "$work/grayscale"
"$work/grayscale" tests/atlas/atlas_case.txt "$work/layout.txt" "$work/atlas.pgm"
diff -u tests/atlas/atlas_expected.txt "$work/layout.txt"
diff -u tests/atlas/atlas_expected.pgm "$work/atlas.pgm"

echo "== 4/5 independent layout and pixel verifier =="
# This binary does not include the packing code: it re-derives geometry and
# texel mapping itself, straight from the source textures and output files.
"$CXX" $FLAGS tests/atlas/verify_atlas.cpp -o "$work/verify"
"$work/verify" tests/atlas/atlas_case.txt "$work/layout.txt" "$work/atlas.pgm"

echo "== 5/5 original example still builds and reproduces its output =="
"$CXX" $FLAGS -Isrc example/main.cpp -o "$work/example"
"$work/example" > "$work/example.out"
diff -u tests/example_expected_output.txt "$work/example.out"

echo "all incremental atlas checks passed"
