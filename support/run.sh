#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) test_src=support/baseline.cc; extra_src= ;;
 target) test_src=Tests/arm_interwork_cases.cc; extra_src=support/main.cc
 test -s "$test_src" || { echo "missing nonempty $test_src" >&2; exit 64; } ;;
 *) exit 64 ;;
esac
cmake -S . -B .offline-build -DARMIPS_USE_STD_FILESYSTEM=ON -DARMIPS_LIBRARY_ONLY=ON -DARMIPS_REGEXP=OFF -DCMAKE_BUILD_TYPE=Release > .configure.log
cmake --build .offline-build --target armips -j4 > .build.log
c++ -std=c++17 -O1 -DARMIPS_USE_STD_FILESYSTEM -I. -Iext/tinyformat "$test_src" $extra_src .offline-build/libarmips.a -o .offline-build/run
.offline-build/run
