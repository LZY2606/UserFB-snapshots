#!/bin/bash
set -euo pipefail
command -v cmake >/dev/null
command -v c++ >/dev/null
cmake --version | head -n 1
c++ --version | head -n 1
printf 'LOCAL_TOOLCHAIN_READY C++17 CMake>=3.16.3 Threads\n'
