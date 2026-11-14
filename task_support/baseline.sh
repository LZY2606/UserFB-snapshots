#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$work"' EXIT
export LC_ALL=C
c++ -std=c++17 -O2 -DNO_OMP -I src task_support/baseline.cpp -o "$work/baseline"
"$work/baseline"
