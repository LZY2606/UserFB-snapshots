#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -Iinclude -Isrc src/simdjson.cpp harness/baseline.cpp -pthread -o "$build/baseline"
"$build/baseline"
