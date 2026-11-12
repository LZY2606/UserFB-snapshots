#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -Dcimg_display=0 -I. task_support/baseline.cpp -o "$build/baseline"
"$build/baseline"
