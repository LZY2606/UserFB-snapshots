#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s tests/target_cases.cpp || { echo 'missing tests/target_cases.cpp' >&2; exit 2; }
mkdir -p .build
test -s src/TheRestOfYourLife/light_path_expression.cpp
c++ -std=c++11 -O2 -Isrc -Isrc/TheRestOfYourLife -Isupport tests/target_cases.cpp support/main.cpp src/TheRestOfYourLife/light_path_expression.cpp -o .build/target
.build/target
