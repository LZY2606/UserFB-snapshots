#!/bin/sh
# Build and run the sostenuto (CC66) tests for tsf.h as C99 and as C++11.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/test_sostenuto.c"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
OUT="${TMPDIR:-/tmp}/tsf_sostenuto_test_$$"
trap 'rm -f "$OUT"' EXIT

echo "== Building as C99 ($CC) =="
"$CC" -std=c99 -Wall -Wextra -O1 -o "$OUT" "$SRC" -lm
echo "== Running C99 build =="
"$OUT"

echo "== Building as C++11 ($CXX) =="
"$CXX" -std=c++11 -Wall -Wextra -O1 -x c++ -o "$OUT" "$SRC" -lm
echo "== Running C++11 build =="
"$OUT"

echo "ALL SOSTENUTO TESTS PASSED"
