#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
for f in tests/ssr_cases.cpp src/renderer/ReflectionBuffer.h src/renderer/ReflectionBuffer.cpp src/renderer/ScreenSpaceReflection.h src/renderer/ScreenSpaceReflection.cpp src/examples/ReflectionPreview.cpp; do
  test -s "$f" || { echo "MISSING_OR_EMPTY $f" >&2; exit 2; }
done
mkdir -p build
b=$(mktemp -d "$PWD/build/.ssr.XXXXXX")
export TMPDIR="$b"
trap 'rm -rf "$b"' EXIT
sources=(src/renderer/VertexProcessor.cpp src/renderer/PolyClipper.cpp src/renderer/LineClipper.cpp src/renderer/ReflectionBuffer.cpp src/renderer/ScreenSpaceReflection.cpp)
"${CXX:-c++}" -std=c++17 -O2 -I. -Isrc/renderer -Isupport support/target_main.cpp tests/ssr_cases.cpp "${sources[@]}" -o "$b/target"
"$b/target"
"${CXX:-c++}" -std=c++17 -O2 -I. -Isrc/renderer src/examples/ReflectionPreview.cpp "${sources[@]}" -o "$b/preview"
out=${1:-build/reflection-preview}
mkdir -p "$out"
"$b/preview" "$out"
for image in original.ppm reflected.ppm; do
  test -s "$out/$image" || { echo "MISSING_IMAGE $image" >&2; exit 4; }
  test "$(head -n 1 "$out/$image")" = P6 || { echo "INVALID_PPM $image" >&2; exit 4; }
done
bash support/baseline.sh
echo SSR_TARGET_OK
