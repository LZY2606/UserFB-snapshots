#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-build.XXXXXX")
trap 'rm -rf "$b"' EXIT

if [[ "${1:-target}" == baseline ]]; then
 c++ -std=c++17 -O2 -Iagg-src/include checks/baseline.cpp agg-src/src/agg_curves.cpp agg-src/src/agg_trans_affine.cpp -lm -o "$b/base"
 "$b/base"
else
 c++ -std=c++17 -O2 -Iagg-src/include checks/target.cpp checks/driver.cpp agg-src/src/agg_curves.cpp agg-src/src/agg_trans_affine.cpp -lm -o "$b/target"
 "$b/target" rational clip topology render
 bash checks/run.sh baseline
fi
