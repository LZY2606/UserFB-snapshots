#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S checks -B build-filter -DCMAKE_BUILD_TYPE=Release
cmake --build build-filter --parallel 4
