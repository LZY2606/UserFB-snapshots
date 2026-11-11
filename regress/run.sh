#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1
test -s "$1"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
sources=()
for src in libsrc/*.cc; do
 case "$src" in libsrc/GLCamera.cc|libsrc/GLManager.cc) continue;; esac
 sources+=("$src")
done
c++ -std=c++11 -O0 -Wno-deprecated-declarations -I include "$1" "${sources[@]}" -o "$build/check"
"$build/check"
