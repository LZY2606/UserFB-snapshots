#!/usr/bin/env bash
set -euo pipefail
: "${build:?caller must set build}"
c++ -std=c++17 -O1 -DWITH_NULL -Iinclude -Ichecks "$@" src/core/*.cpp src/backend/null/soloud_null.cpp src/filter/soloud_duckfilter.cpp -pthread -o "$build/run"
