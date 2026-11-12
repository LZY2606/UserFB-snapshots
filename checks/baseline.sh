#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cxx.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O2 -I. checks/baseline.cpp -o "$b/baseline"
"$b/baseline"
