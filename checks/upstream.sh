#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -DWITH_NULL -Iinclude demos/null/main.cpp src/core/*.cpp src/backend/null/soloud_null.cpp src/audiosource/speech/*.cpp -pthread -o "$build/null-demo"
"$build/null-demo" > "$build/output"
lines=$(wc -l < "$build/output" | tr -d ' ')
test "$lines" -gt 0
printf 'UPSTREAM_NULL_DEMO lines=%s completed=1\n' "$lines"
