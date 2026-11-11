#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
cmake -S checks -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target contact_baseline -j 2
"$build/contact_baseline"
