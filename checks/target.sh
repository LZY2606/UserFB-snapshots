#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cxx.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O2 -Iinclude checks/target_main.cpp checks/target_tests.cpp -o "$b/target"
"$b/target"
bash checks/baseline.sh
