#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build/lib" -DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF -DLIBDEFLATE_BUILD_TESTS=OFF >/dev/null
cmake --build "$build/lib" --target libdeflate_static --parallel 2 >/dev/null
if [[ ${1:-baseline} == target ]]; then
 cc -std=c99 -O2 -I. -Iharness harness/main.c harness/dictionary_target.c "$build/lib/libdeflate.a" -o "$build/target"
 "$build/target" > "$build/result.log"; cat "$build/result.log"
 grep -qx 'LIBDEFLATE_TARGET_COMPLETE groups=4' "$build/result.log"
elif [[ ${1:-baseline} != baseline ]]; then exit 2; fi
cc -std=c99 -O2 -I. harness/baseline.c "$build/lib/libdeflate.a" -o "$build/baseline"
"$build/baseline"
