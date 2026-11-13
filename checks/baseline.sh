#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d checks/.build.XXXXXX)
trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
"${CXX:-c++}" -std=c++17 -O1 -g -Iinclude checks/baseline.cpp "$build/libdumb.a" -lm ${CHECK_CFLAGS:-} -o "$build/baseline"
"$build/baseline" "$@"
