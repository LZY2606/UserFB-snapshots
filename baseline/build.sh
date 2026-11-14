#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
# Content-based cache includes all production translation units and headers.
key=$(find libShapeOp/src vendor/eigen/Eigen vendor/eigen/unsupported/Eigen baseline -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.sh' -o -name '*ForwardDeclarations*' \) -exec cksum {} \; | LC_ALL=C sort | cksum)
if [ -f .task-build/build-key ] && [ "$(cat .task-build/build-key)" = "$key" ] && [ -s .task-build/libshapeop.a ] && [ -x .task-build/baseline ]; then exit 0; fi
rm -f .task-build/libshapeop.a
objects=()
for file in libShapeOp/src/*.cpp; do
 case "$file" in */API.cpp) continue;; esac
 obj=".task-build/$(basename "${file%.cpp}").o"
 "${CXX:-c++}" -std=c++17 -O1 -DNDEBUG -IlibShapeOp/src -Ivendor/eigen -c "$file" -o "$obj"
 objects+=("$obj")
done
ar rcs .task-build/libshapeop.a "${objects[@]}"
"${CXX:-c++}" -std=c++17 -O1 -IlibShapeOp/src -Ivendor/eigen baseline/current.cpp .task-build/libshapeop.a -o .task-build/baseline
printf '%s\n' "$key" > .task-build/build-key
