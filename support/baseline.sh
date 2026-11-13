#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
c++ -std=c++11 -O2 -Isrc -Isrc/TheRestOfYourLife support/baseline.cpp -o .build/baseline
.build/baseline
