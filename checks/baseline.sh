#!/usr/bin/env bash
set -euo pipefail
test -s .deps/eigen/Eigen/Core
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S checks -B "$b" -DGC_EIGEN_LOCATION="$PWD/.deps/eigen" -DSUITESPARSE=OFF -DCMAKE_CXX_FLAGS='-O0 -g0'
cmake --build "$b" --target heat_baseline -j 4
"$b/heat_baseline"
# After the component-isolation fix the disconnected-component contract must now hold: no finite distances on the
# unsourced patch, and the sourced patch must match a standalone solve exactly.
if "$b/heat_baseline" --require-contract; then
  echo GEOMETRYCENTRAL_COMPONENT_CONTRACT_PASS
else
  rc=$?
  echo "Unexpected: component contract still fails (rc=$rc)" >&2
  exit 1
fi
