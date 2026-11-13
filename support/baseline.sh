#!/bin/sh
set -eu
cmake -S support -B .build -DCMAKE_BUILD_TYPE=Release
cmake --build .build --target baseline -j 4
MLT_REPOSITORY="$PWD/support/no-plugins" MLT_DATA="$PWD/support/no-data" .build/baseline
