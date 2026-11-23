# Sourced by baseline/target. Only portable scalar core; no test frameworks.
set -euo pipefail
sources=()
while IFS= read -r p; do sources+=("$p"); done < <(find src/zimg -name '*.cpp' ! -path '*/x86/*' ! -path '*/arm/*' | LC_ALL=C sort)
sources+=(graphengine/graphengine/cpuinfo.cpp graphengine/graphengine/graph.cpp graphengine/graphengine/node.cpp)
c++ -std=c++17 -O1 -pthread -DGRAPHENGINE_IMPL_NAMESPACE=zimg -Isrc/zimg -Igraphengine/include "${sources[@]}" "$1" -o "$2"
