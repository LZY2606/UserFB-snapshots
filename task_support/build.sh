#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/ogg -DBUILD_TESTING=ON -DINSTALL_DOCS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build/ogg -j2
cc -std=c11 -Wall -Wextra -Iinclude -I.task-build/ogg/include task_support/baseline.c .task-build/ogg/libogg.a -o .task-build/baseline
