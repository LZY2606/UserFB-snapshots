#!/bin/bash
set -euo pipefail
c++ -std=c++11 -O2 -I Detour/Include -I task_support "$@" Detour/Source/*.cpp -o "$task_build/run"
