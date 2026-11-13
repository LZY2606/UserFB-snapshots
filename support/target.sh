#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s tests/target_cases.cpp || { echo 'missing tests/target_cases.cpp' >&2; exit 2; }
mkdir -p .build
c++ -std=c++17 -O2 -D__EMSCRIPTEN__ -include cmath -include cstring -I. -Isupport tests/target_cases.cpp support/main.cpp Cpp/Source/Maths.cpp -o .build/target
.build/target
