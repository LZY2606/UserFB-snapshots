#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S checks -B .check/baseline -DCMAKE_BUILD_TYPE=Debug -DXM_RAMPING=ON
cmake --build .check/baseline --target check_baseline -j 4
cmake -S tests -B .check/old -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_STANDARD=23 -DXM_VERBOSE=OFF
cmake --build .check/old -j 4
