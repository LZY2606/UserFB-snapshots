#!/usr/bin/env bash
set -euo pipefail
b=${1:?build directory}
cmake -S . -B "$b" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF -DWITH_LSR_BINDINGS=OFF -DWITH_OPENMP=OFF -DWITH_CR32S=OFF -DWITH_CR64S=OFF
cmake --build "$b" --target soxr 1-delay-clear -j2
