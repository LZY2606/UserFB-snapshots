#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
source verification/build.sh
c++ -std=c++17 -O2 -I. -Icodec/api/wels verification/baseline.cpp "$b/libopenh264.a" -lpthread -o "$b/probe"
"$b/probe"
