#!/bin/bash
set -euo pipefail
c++ -std=c++11 -O2 -DWITH_NULL -I include -I task_support "$@" src/core/*.cpp src/backend/null/soloud_null.cpp -pthread -o "$task_build/run"
