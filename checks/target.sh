#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cxx.XXXXXX")
trap 'rm -rf "$b"' EXIT
# -ffp-contract=off keeps the watertight edge-function multiplies and
# subtracts as separate roundings, matching the reference model and making
# the double-precision edge fallback observable independent of host FMA.
c++ -std=c++17 -O2 -ffp-contract=off -I. checks/target_main.cpp checks/target_tests.cpp -o "$b/target"
"$b/target"
bash checks/baseline.sh
bash checks/legacy.sh
