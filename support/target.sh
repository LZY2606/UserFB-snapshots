#!/usr/bin/env bash
set -euo pipefail
test -s tests/source_stream.c
test -s examples/packet_sources.c
build=$(mktemp -d)
trap 'cmake -E remove_directory "$build"' EXIT
cmake -S . -B "$build" -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF
cmake --build "$build" --parallel 2
cc -std=c99 -Wall -Wextra -Iinclude -I"$build/include" tests/source_stream.c "$build/libogg.a" -o "$build/source_stream"
"$build/source_stream" "$build/sample.ogg" | tee "$build/result.log"
for marker in SOURCE_REASSEMBLY_OK SOURCE_HOLES_OK SOURCE_PEEK_RESET_OK SOURCE_OFFSETS_OK; do
 grep -Fxq "$marker" "$build/result.log"
done
cc -std=c99 -Wall -Wextra -Iinclude -I"$build/include" examples/packet_sources.c "$build/libogg.a" -o "$build/example"
"$build/example" "$build/sample.ogg" | tee "$build/example.log"
grep -Fxq "packet=0 bytes=70000 spans=2" "$build/example.log"
"$build/test_bitwise"
"$build/test_framing"
echo OGG_SOURCE_TARGET_OK
