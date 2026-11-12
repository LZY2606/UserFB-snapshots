#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-build.XXXXXX")
trap 'rm -rf "$b"' EXIT

if [[ "${1:-target}" == baseline ]]; then
 c++ -DMSDFGEN_PUBLIC= -DMSDFGEN_USE_CPP11 -std=c++17 -O2 -I. checks/baseline.cpp core/*.cpp -lm -o "$b/base"
 "$b/base"
else
 c++ -DMSDFGEN_PUBLIC= -DMSDFGEN_USE_CPP11 -std=c++17 -O2 -I. checks/target.cpp checks/driver.cpp core/*.cpp -lm -o "$b/target"
 "$b/target" distance endpoints contours pruning
 bash checks/run.sh baseline
fi
