#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++20 -O1 -pthread -I src devcheck/baseline.cpp -o "$b/baseline"
"$b/baseline"
c++ -std=c++20 -O1 -pthread -I src test/simple_example.cpp -o "$b/simple"
"$b/simple"
c++ -std=c++20 -O1 -pthread -I src test/serialize.cpp -o "$b/serialize"
(cd "$b"; ./serialize)
echo BVH_SELECTED_UPSTREAM_OK
