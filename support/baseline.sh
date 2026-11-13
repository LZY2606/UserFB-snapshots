#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
${CXX:-c++} -std=c++14 -O2 -I . -I libs -I libs/emilib libs.cpp -pthread -ldl support/baseline.cpp -o "$build_dir/baseline"
"$build_dir/baseline"
