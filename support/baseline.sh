#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
b=$(mktemp -d "$PWD/build/.baseline.XXXXXX")
export TMPDIR="$b"
trap 'rm -rf "$b"' EXIT
sources=(src/renderer/VertexProcessor.cpp src/renderer/PolyClipper.cpp src/renderer/LineClipper.cpp)
"${CXX:-c++}" -std=c++17 -O2 -Isrc/renderer support/baseline.cpp "${sources[@]}" -o "$b/base"
"$b/base"
"${CXX:-c++}" -std=c++17 -O2 -Isrc/renderer -Isupport/headless src/examples/VertexProcessorTest.cpp "${sources[@]}" -o "$b/upstream-example"
"$b/upstream-example"
