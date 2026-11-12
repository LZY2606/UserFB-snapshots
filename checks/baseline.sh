#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -ffp-contract=off -Isrc checks/baseline.cpp -o "$build/baseline"
"$build/baseline"
