#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$work"' EXIT
export LC_ALL=C
sources=()
for file in src/*.cpp; do
  if [ "$file" != src/MiniLight.cpp ]; then sources+=("$file"); fi
done
c++ -std=c++17 -O2 -I src task_support/baseline.cpp "${sources[@]}" -o "$work/baseline"
"$work/baseline"
