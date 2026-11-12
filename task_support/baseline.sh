#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -I. task_support/baseline.cpp rg_etc1.cpp -o "$build/baseline"
"$build/baseline"
