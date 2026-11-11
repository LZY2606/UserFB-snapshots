#!/usr/bin/env bash
set -euo pipefail
src=$PWD
test_source=${1:-harness/baseline.c}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cd "$build"
"$src/configure" --disable-shared --enable-static --disable-asm --disable-fontconfig --disable-coretext --disable-require-system-font-provider --disable-libunibreak
make -j2
./libtool --mode=link cc -I"$src/libass" "$src/$test_source" libass/libass.la -o baseline
cd "$src"
"$build/baseline"
