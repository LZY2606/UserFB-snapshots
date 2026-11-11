#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'find "$b" -depth -delete' EXIT
cmake -S tests -B "$b/build" -DUSE_OMP=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$b/build" --target tests -j4
(cd "$b" && "$b/build/tests" --testBASE --testCG --testTPR)
printf 'vidstab transform/chroma/ownership baseline: ok\n'
