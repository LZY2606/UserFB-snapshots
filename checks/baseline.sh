#!/usr/bin/env bash
set -euo pipefail
test -s .deps/eigen/Eigen/Core
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S checks -B "$b" -DGC_EIGEN_LOCATION="$PWD/.deps/eigen" -DSUITESPARSE=OFF -DCMAKE_CXX_FLAGS='-O0 -g0'
cmake --build "$b" --target heat_baseline -j 4
"$b/heat_baseline"
if "$b/heat_baseline" --require-contract; then
  echo 'Unexpected: original component contract already passes' >&2
  exit 1
else
  rc=$?
  test "$rc" -eq 1
  echo GEOMETRYCENTRAL_TARGET_CONTRACT_FAIL_CONFIRMED
fi
