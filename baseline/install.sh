#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
deps="$root/baseline/.deps"
mkdir -p "$deps/downloads" "$deps/src" "$deps/prefix"
fetch() {
  local name=$1 url=$2 expected=$3
  if [ ! -f "$deps/downloads/$name" ]; then curl -fsSL --retry 2 "$url" -o "$deps/downloads/$name"; fi
  local actual
  actual=$(shasum -a 256 "$deps/downloads/$name" | awk '{print $1}')
  test "$actual" = "$expected" || { echo "DEPENDENCY_HASH_MISMATCH $name" >&2; exit 1; }
  echo "DEPENDENCY_SHA256_OK $name $actual"
}
fetch zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
# The sandbox cannot reach SourceForge; use the upstream GitHub mirror of the
# same 2.1-3 release.
fetch CUnit-2.1-3.tar.gz https://github.com/jacklicn/CUnit/archive/refs/tags/v2.1-3.tar.gz b9ba84b54b916d81608572985d558b23bdcd7739180852273948de5cdc3a2cca
if [ ! -f "$deps/prefix/lib/libz.a" ]; then
  tar -xzf "$deps/downloads/zlib-1.3.1.tar.gz" -C "$deps/src"
  cmake -S "$deps/src/zlib-1.3.1" -B "$deps/zlib-build" -DCMAKE_INSTALL_PREFIX="$deps/prefix" -DZLIB_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build "$deps/zlib-build" --target install -j 4
fi
if [ ! -f "$deps/prefix/lib/libcunit.a" ]; then
  tar -xzf "$deps/downloads/CUnit-2.1-3.tar.gz" -C "$deps/src"
  # Build the genuine framework and Basic frontend, without autoconf/libtool.
  unit="$deps/src/CUnit-2.1-3"
  mkdir -p "$deps/cunit-build" "$deps/prefix/include/CUnit"
  cp "$unit"/CUnit/Headers/*.h "$deps/prefix/include/CUnit/"
  sed 's/@VERSION@/2.1/g; s/@RELEASE@/3/g' "$unit/CUnit/Headers/CUnit.h.in" > "$deps/prefix/include/CUnit/CUnit.h"
  for source in "$unit"/CUnit/Sources/Framework/*.c "$unit/CUnit/Sources/Basic/Basic.c"; do
    cc -std=c99 -O2 -I "$deps/prefix/include/CUnit" -c "$source" -o "$deps/cunit-build/$(basename "${source%.c}").o"
  done
  ar rcs "$deps/prefix/lib/libcunit.a" "$deps"/cunit-build/*.o
  cp "$unit/COPYING" "$deps/prefix/CUnit-COPYING"
fi
cmake -S . -B baseline/.build -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DCMAKE_BUILD_TYPE=Release -DZLIB_INCLUDE_DIR="$deps/prefix/include" -DZLIB_LIBRARY="$deps/prefix/lib/libz.a"
cmake --build baseline/.build -j 4
cc -std=c99 -O2 -I src/hrtf -I baseline/.build/src baseline/hrtf_witness.c baseline/.build/src/libmysofa.a "$deps/prefix/lib/libz.a" -lm -o baseline/.build/hrtf_witness
cc -std=c99 -O2 -I src/hrtf -I baseline/.build/src -I "$deps/prefix/include" baseline/upstream_selected.c src/tests/tools.c src/tests/interpolate.c baseline/.build/src/libmysofa.a "$deps/prefix/lib/libcunit.a" "$deps/prefix/lib/libz.a" -lm -o baseline/.build/upstream_selected
printf 'DEPENDENCIES_READY zlib=1.3.1 CUnit=2.1-3\n'
