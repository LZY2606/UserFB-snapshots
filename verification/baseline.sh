#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -I. lodepng.cpp verification/baseline.cpp -o "$b/probe"
"$b/probe"
c++ -std=c++11 -O2 -I. lodepng.cpp lodepng_util.cpp lodepng_unittest.cpp -o "$b/old-tests"
"$b/old-tests" > "$b/old.log" 2>&1 || { tail -n 25 "$b/old.log"; exit 1; }
tail -n 3 "$b/old.log"
echo LODEPNG_UPSTREAM_SUITE_PASS
