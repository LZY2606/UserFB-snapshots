#!/bin/bash
set -euo pipefail
cmake -S . -B "$b/build" -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_JPIP=OFF -DCMAKE_BUILD_TYPE=Release > "$b/configure.log" 2>&1 || { tail -n 25 "$b/configure.log"; exit 1; }
cmake --build "$b/build" --target openjp2 -j4 > "$b/build.log" 2>&1 || { tail -n 25 "$b/build.log"; exit 1; }
