#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S checks -B build-check -DCMAKE_BUILD_TYPE=Debug
cmake --build build-check --target check_baseline gtest_runner -j 4
./build-check/check_baseline
(cd build-check/pmp-build/tests && ./gtest_runner --gtest_filter='RemeshingTest.*:SurfaceMeshTest.*')
