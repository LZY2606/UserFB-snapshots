#!/bin/sh
# Regression test for signalsmith::feedback::Reverb4 (feedback.h).
# Run from the repo root:  sh tests/run_fdn.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
${CXX:-c++} -std=c++11 -O2 -Wall -Wextra -I. tests/fdn_tests.cpp -o build/fdn_tests
./build/fdn_tests build/fdn_trace.csv
