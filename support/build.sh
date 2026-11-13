#!/bin/sh
set -eu
cmake -S . -B .build -DARIBCC_NO_RENDERER=ON -DARIBCC_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .build -j 4
