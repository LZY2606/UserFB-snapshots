#!/bin/bash
set -euo pipefail
out=$1
objects=()
for source in src/*.c; do
 name=$(basename "$source" .c)
 ${CC:-cc} -std=c99 -O1 -Iinclude -Isrc -c "$source" -o "$out/$name.o"
 objects+=("$out/$name.o")
done
ar rcs "$out/libsource.a" "${objects[@]}"
${CC:-cc} -std=c99 -Iinclude task_support/baseline.c "$out/libsource.a" -lm -o "$out/baseline"
${CC:-cc} -std=c99 -Iinclude t/harness.c "$out/libsource.a" -lm -o "$out/harness"
for f in t/60screen_ascii.test t/61screen_unicode.test t/64screen_pen.test t/65screen_protect.test t/69screen_pushline.test; do
 perl t/run-test.pl --executable "$out/harness" "$f"
done
echo UPSTREAM_SCREEN_FIVE_FILES_OK
