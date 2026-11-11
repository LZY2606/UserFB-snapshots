#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DCMAKE_BUILD_TYPE=Release -DARIBCC_BUILD_TESTS=OFF -DARIBCC_USE_EMBEDDED_FREETYPE=OFF
cmake --build "$b/build" --target aribcaption -j 4
if [ "$(uname -s)" = Darwin ]; then
  c++ -std=c++17 -O1 -Iinclude -Isrc -I"$b/build/include" verification/baseline.cc "$b/build/libaribcaption.a" -framework CoreText -framework CoreGraphics -framework CoreFoundation -o "$b/witness"
else
  c++ -std=c++17 -O1 -Iinclude -Isrc -I"$b/build/include" verification/baseline.cc "$b/build/libaribcaption.a" $(pkg-config --libs freetype2 fontconfig) -o "$b/witness"
fi
"$b/witness"
