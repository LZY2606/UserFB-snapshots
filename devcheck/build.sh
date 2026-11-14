#!/bin/bash
set -euo pipefail
build_case() {
 if [[ -f jpeg_progressive.cpp ]]; then
  c++ -std=c++11 -O2 -Idevcheck -I. jpge.cpp jpgd.cpp jpeg_progressive.cpp "$@" -o "$b/case"
 else
  c++ -std=c++11 -O2 -Idevcheck -I. jpge.cpp jpgd.cpp "$@" -o "$b/case"
 fi
}
