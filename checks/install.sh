#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .cff2-check
${CXX:-c++} -std=c++11 -O0 -g0 -pthread -Isrc -c src/harfbuzz-subset.cc -o .cff2-check/hb.o
${CXX:-c++} -std=c++11 -O0 -g0 -pthread -Isrc checks/baseline.cpp .cff2-check/hb.o -o .cff2-check/baseline
printf '%s\n' CFF2_INSTALL_OK
