#!/bin/bash
set -euo pipefail
R=$(pwd)
B=${CHECK_BUILD_DIR:-$(mktemp -d "$R/.build.XXXXXX")}
if [[ -z ${CHECK_BUILD_DIR:-} ]]; then trap 'rm -rf "$B"' EXIT; fi
mkdir -p "$B"
cmake -S "$R" -B "$B" -DCMAKE_BUILD_TYPE=Release -DBUILD_APPS=OFF -DBUILD_SHARED_LIBRARY=OFF -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build "$B" --target ap4 -j4
INC=(-I"$R/Source/C++/Core" -I"$R/Source/C++/Codecs" -I"$R/Source/C++/Crypto" -I"$R/Source/C++/MetaData" -I"$R/support")
compile() { c++ -std=c++17 "${INC[@]}" "$1" "$B/libap4.a" -o "$2"; }
