#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d); trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -Iinclude regress/current.cpp src/FrameGraph.cpp src/PassNode.cpp src/GraphvizWriter.cpp -o "$build/current"
"$build/current"
