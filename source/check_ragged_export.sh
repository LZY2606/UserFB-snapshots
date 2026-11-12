#!/usr/bin/env bash
#
# Builds and runs the ragged (mixed triangle/quad polygon) export regression
# tests in an isolated temporary directory. Run from the repository root:
#
#   bash source/check_ragged_export.sh
#
# On success the test binary prints "ragged mesh export PASS".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/ragged_export.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

CXX_BIN="${CXX:-c++}"

"$CXX_BIN" -std=c++17 -Wall -Wpedantic -I"$ROOT/source" \
    "$ROOT/source/tinyply.cpp" \
    "$ROOT/source/polygon_subset.cpp" \
    "$ROOT/source/ragged_export_tests.cpp" \
    -o "$WORKDIR/ragged_export_tests"

"$WORKDIR/ragged_export_tests"
