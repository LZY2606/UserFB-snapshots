#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -I. checks/baseline.cpp -o "$b/baseline"
"$b/baseline"
c++ -std=c++11 -O2 -I. test/regression/possible-accuracy-problem-30/main.cc -o "$b/old"
"$b/old" > "$b/old.log"
grep -F 'We have the expected result' "$b/old.log"
echo NANORT_OLD_REGRESSION_OK
