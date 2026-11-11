#!/usr/bin/env bash
#
# Builds the library in a temporary directory and runs the existing assertion
# test suite (meshoptdemo) together with the group simplification regression.
#
# Usage (from the repository root): bash demo/run_group_regression.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/group-regression.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

echo "Building in $BUILD_DIR"

# build both the regular demo (assertion tests) and the standalone group regression
make -C "$REPO_ROOT" BUILD="$BUILD_DIR" "$BUILD_DIR/meshoptdemo" "$BUILD_DIR/group_regression"

echo "Running existing assertion tests"
"$BUILD_DIR/meshoptdemo"

echo "Running group simplification regression"
"$BUILD_DIR/group_regression"

echo "group simplification PASS"
