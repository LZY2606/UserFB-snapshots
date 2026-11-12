#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/liqbaseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
for f in libimagequant blur mediancut mempool nearest pam kmeans; do
 cc -std=c99 -O1 -DUSE_SSE=0 -I. -c "$f.c" -o "$build/$f.o"
done
c++ -std=c++17 -O1 -I. task_support/baseline.cpp "$build/"*.o -o "$build/baseline"
"$build/baseline"
