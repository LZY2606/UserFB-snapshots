#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
make -j4 CXX="${CXX:-c++}" CXXFLAGS='-std=c++11 -O0 -DNDEBUG -w' CPPCHK_GLIBCXX_DEBUG= cppcheck
"${CXX:-c++}" -std=c++11 -O0 -I. -Ilib -Icli -Iexternals/picojson -Iexternals/simplecpp -Iexternals/tinyxml2 task_support/baseline.cpp lib/*.o externals/simplecpp/*.o externals/tinyxml2/*.o -pthread -o .task-build/baseline
