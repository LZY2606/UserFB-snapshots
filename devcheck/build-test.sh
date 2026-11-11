#!/usr/bin/env bash
set -euo pipefail
# Usage: bash devcheck/build-test.sh SOURCE_FILE OUTPUT [LIBRARY_SOURCE_ROOT]
test "$#" -ge 2
here=$(cd "$(dirname "$0")/.." && pwd)
lib=${3:-$here}
file=$1
out=$2
test "$(pkg-config --modversion opus)" = 1.6.1
read -r -a cf <<< "$(pkg-config --cflags opus)"
read -r -a lb <<< "$(pkg-config --libs opus)"
build=$(mktemp -d "$here/.build.XXXXXX")
trap 'rm -rf "$build"' EXIT
flags=(-std=c99 -O1 -I"$lib/include" -I"$lib/src" -DOUTSIDE_SPEEX -DRANDOM_PREFIX=libopusenc '-DPACKAGE_NAME="libopusenc"' '-DPACKAGE_VERSION="0.2.1"')
for name in opusenc ogg_packer opus_header picture random resample unicode_support; do
  clang "${flags[@]}" "${cf[@]}" -c "$lib/src/$name.c" -o "$build/$name.o"
done
case "$file" in
  *.cc|*.cpp) clang++ -std=c++17 -O1 -I"$lib/include" -I"$lib/src" "${cf[@]}" "$file" "$build"/*.o "${lb[@]}" -lm -o "$out" ;;
  *.c) clang -std=c99 -O1 -I"$lib/include" -I"$lib/src" "${cf[@]}" "$file" "$build"/*.o "${lb[@]}" -lm -o "$out" ;;
  *) echo 'unsupported test source' >&2; exit 2 ;;
esac
