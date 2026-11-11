#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/tuning-range-demo.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++20 -Iinclude checks/range_demo.cpp -o "$build/range_demo"
"$build/range_demo"
