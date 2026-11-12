#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -Ishared/DSPFilters/include checks/baseline.cpp shared/DSPFilters/source/*.cpp -o "$b/baseline"
"$b/baseline"
