#!/bin/sh
set -eu
bash support/build.sh
c++ -std=c++17 -Iinclude -I.build/include -Itest/sample_data/include support/baseline.cpp .build/libaribcaption.a -o .build/baseline
.build/baseline
