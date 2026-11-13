#!/bin/sh
set -eu
test -s src/tests/sample_curve.c || { echo 'missing src/tests/sample_curve.c'; exit 2; }
cmake -S support -B .build -DCMAKE_BUILD_TYPE=Release
cmake --build .build --target target_tests --clean-first -j 4
MLT_REPOSITORY="$PWD/support/no-plugins" MLT_DATA="$PWD/support/no-data" .build/target_tests
