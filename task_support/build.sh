#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --target motion_baseline -j 2
