#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
mkdir -p .build/zlib .build/engine .build/deps
if test ! -f .build/deps/lib/libz.a; then
 (cd .build/zlib; CFLAGS='-O0' "$root/task_support/zlib/configure" --static --prefix="$root/.build/deps"; make -j4; make install)
fi
if test ! -f .build/engine/Makefile; then
 (cd .build/engine; CFLAGS='-O0' "$root/configure" --disable-auto-features --disable-https --without-iconv --disable-selftests --disable-shared --enable-static --disable-example-libs --with-zlib="$root/.build/deps")
fi
make -C .build/engine/src -j4 httrack
