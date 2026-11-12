#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/playable_clip.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -Iinclude checks/playable_clip.cpp src/*.cpp -o "$b/playable_clip"
"$b/playable_clip" "$b"
