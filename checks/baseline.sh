#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++20 -Iinclude checks/baseline.cpp -o "$build/baseline"
"$build/baseline"
