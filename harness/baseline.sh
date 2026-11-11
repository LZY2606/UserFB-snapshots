#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -I . QuickHull.cpp harness/baseline.cpp -o "$b/baseline"
"$b/baseline"
c++ -std=c++14 -O1 QuickHull.cpp Tests/main.cpp Tests/QuickHullTests.cpp -o "$b/upstream"
"$b/upstream" > "$b/upstream.log" 2>&1
cat "$b/upstream.log"
if grep -q 'test failed' "$b/upstream.log"; then exit 1; fi
grep -q 'QuickHull tests succesfully passed.' "$b/upstream.log"
echo QUICKHULL_UPSTREAM_OK
