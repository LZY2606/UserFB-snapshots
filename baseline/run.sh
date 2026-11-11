#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-check
"${CXX:-c++}" -std=c++20 -O2 -pthread -I. baseline/static_instances.cpp -o build-check/static_instances
build-check/static_instances
