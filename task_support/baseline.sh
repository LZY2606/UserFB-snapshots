#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/agg-old-stroke.XXXXXX")
trap 'rm -rf "$build"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Iagg-src/include task_support/baseline.cpp agg-src/src/agg_curves.cpp agg-src/src/agg_vcgen_stroke.cpp -o "$build/baseline"
"$build/baseline"
