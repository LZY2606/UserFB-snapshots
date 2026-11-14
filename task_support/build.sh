#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/srt -DENABLE_ENCRYPTION=OFF -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_LOGGING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/srt -j 2
