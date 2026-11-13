#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/uf-build.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DMATIO_WITH_HDF5=OFF -DMATIO_MAT73=OFF -DMATIO_WITH_ZLIB=OFF -DMATIO_USE_CONAN=OFF -DMATIO_SHARED=OFF -DMATIO_BUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target matio -j 2
lib=$(find "$build" -name libmatio.a -print -quit)
cc -std=c11 -Isrc -I"$build/src" support/baseline.c "$lib" -lm -o "$build/baseline"
"$build/baseline" "$build/baseline.mat"
if [[ ${1:-baseline} == target ]]; then
 cc -std=c11 -Isrc -I"$build/src" -Isupport support/runner.c regress_tests/regression.c "$lib" -lm -o "$build/target"
 (cd "$build"; "$build/target") | tee "$build/target.log"
 grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* groups=3$' "$build/target.log"
fi
