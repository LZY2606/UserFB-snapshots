#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/target.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sh support/baseline.sh
${CXX:-c++} -std=c++14 -O2 -I . -I libs -I libs/emilib libs.cpp -pthread -ldl tests/constrained_test.cpp -o "$build_dir/target"
"$build_dir/target"
${CXX:-c++} -std=c++14 -O2 -I . -I libs -I libs/emilib libs.cpp -pthread -ldl examples/constrained.cpp -o "$build_dir/example"
"$build_dir/example" "$build_dir/result.ppm"
test -s "$build_dir/result.ppm"
