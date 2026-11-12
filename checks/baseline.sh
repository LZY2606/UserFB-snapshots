#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -I. -ISource -ISource/Utility checks/baseline.cpp Source/Effects/overdrive.cpp Source/Effects/wavefolder.cpp -o "$build/baseline"
"$build/baseline"
