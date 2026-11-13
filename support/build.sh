#!/bin/sh
set -eu
mkdir -p .build
c++ -std=c++17 -O2 -include cstdlib -include cassert -Isrc/renderer -Isupport "$@" src/renderer/VertexProcessor.cpp src/renderer/PolyClipper.cpp src/renderer/LineClipper.cpp -o .build/check
