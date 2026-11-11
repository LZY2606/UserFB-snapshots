#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'cmake -E remove_directory "$build"' EXIT
cmake -S . -B "$build" -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF
cmake --build "$build" --parallel 2
cc -std=c99 -Wall -Wextra -Iinclude -I"$build/include" support/baseline.c "$build/libogg.a" -o "$build/baseline"
"$build/baseline"
"$build/test_bitwise"
"$build/test_framing"
echo OGG_OLD_SELFTESTS_OK
