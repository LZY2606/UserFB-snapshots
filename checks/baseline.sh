#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -I. checks/baseline.cpp -o "$b/check"
"$b/check"
