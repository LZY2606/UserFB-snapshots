#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -Iinclude checks/baseline.cpp src/*.cpp -o "$b/baseline"
"$b/baseline" "$b/roundtrip.mid"
