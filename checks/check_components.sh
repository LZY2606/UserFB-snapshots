#!/usr/bin/env bash
set -euo pipefail

# Eigen must be available locally (see checks/install.sh).
test -s .deps/eigen/Eigen/Core

b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT

# Compile in a temporary build directory.
cmake -S checks -B "$b" -DGC_EIGEN_LOCATION="$PWD/.deps/eigen" -DSUITESPARSE=OFF \
      -DCMAKE_CXX_FLAGS='-O0 -g0' >/dev/null
cmake --build "$b" --target heat_components -j 4

# The program must really run, execute a nonzero number of scenarios, and return zero.
out=$("$b/heat_components")
printf '%s\n' "$out"

printf '%s\n' "$out" | grep -q '^PASS component-reference$'
printf '%s\n' "$out" | grep -q '^PASS source-kinds$'
printf '%s\n' "$out" | grep -q '^PASS robust-reuse$'

scenarios=$(printf '%s\n' "$out" | sed -n 's/^SCENARIOS=//p')
test -n "$scenarios"
test "$scenarios" -gt 0

printf '%s\n' "$out" | grep -q '^HEAT_COMPONENTS_OK$'
