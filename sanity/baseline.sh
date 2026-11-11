#!/usr/bin/env bash
set -euo pipefail
test -s .deps/eigen/Eigen/Core
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O0 -I include -I .deps/eigen sanity/baseline.cpp -pthread -o "$b/baseline"
"$b/baseline"
