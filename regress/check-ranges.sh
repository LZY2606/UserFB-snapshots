#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d); trap 'rm -rf "$build"' EXIT
test -s tests/ranges.cpp
c++ -std=c++17 -Iinclude tests/ranges.cpp src/FrameGraph.cpp src/PassNode.cpp src/GraphvizWriter.cpp -o "$build/ranges"
"$build/ranges" | tee "$build/output"
for marker in RANGE_INHERITANCE_OK RANGE_CULLING_OK RANGE_LIFETIME_OK RANGE_ERRORS_OK; do grep -qx "$marker" "$build/output"; done
bash regress/run-current.sh
printf 'FRAMEGRAPH_RANGES_OK\n'
