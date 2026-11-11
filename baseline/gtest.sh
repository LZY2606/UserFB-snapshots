#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-old}
shift || true
sh baseline/build.sh
gtest=.deps/googletest/googletest
test -s "$gtest/src/gtest-all.cc" || { echo 'Run sh baseline/install.sh first' >&2; exit 1; }
if [ ! -f .build/gtest-all.o ]; then
 c++ -std=c++14 -O2 -pthread -I"$gtest/include" -I"$gtest" -c "$gtest/src/gtest-all.cc" -o .build/gtest-all.o
 c++ -std=c++14 -O2 -pthread -I"$gtest/include" -I"$gtest" -c "$gtest/src/gtest_main.cc" -o .build/gtest-main.o
fi
if [ "$mode" = old ]; then
 src=Tests/libtess2_test.cc
 filter='Libtess2Test.*:Libtess2CustomAllocTest.*'
else
 src=Tests/boundary_sources_test.cc
 filter='BoundarySources.*'
fi
test -s "$src" || { echo "Missing nonempty $src" >&2; exit 1; }
c++ -std=c++14 -O2 -g -pthread -IInclude -I"$gtest/include" "$src" .build/gtest-all.o .build/gtest-main.o .build/libtess2.a -lm -o ".build/$mode-tests"
".build/$mode-tests" --gtest_color=no --gtest_filter="$filter" "$@"
