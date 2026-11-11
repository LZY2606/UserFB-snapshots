#!/bin/sh
# Compile and run the integer mixing precision regression against both miniaudio distributions:
#   1. The single-header build (implementation pulled in by checks/mix_precision.c).
#   2. The generated split build in extras/miniaudio_split.
# Any numeric or frame-count mismatch makes the C program return non-zero, which fails this script.
set -eu

b=$(mktemp -d "${TMPDIR:-/tmp}/mix_precision.XXXXXX")
trap 'rm -rf "$b"' EXIT

CFLAGS="-std=c99 -Wall -Wextra -O2"

echo "== build 1/2: single-header miniaudio.h =="
cc $CFLAGS -I. checks/mix_precision.c -lm -lpthread -o "$b/mix_single"
"$b/mix_single"

echo
echo "== build 2/2: generated split extras/miniaudio_split =="
# The split implementation is library code with its own warning set; keep -Wextra noise out of the
# test output while still compiling the test translation unit with the strict flags above.
cc -std=c99 -O2 -Iextras/miniaudio_split -DMIX_TEST_SPLIT -c checks/mix_precision.c -o "$b/mix_test.o"
cc -std=c99 -O2 -Wno-unused-function -Iextras/miniaudio_split -c extras/miniaudio_split/miniaudio.c -o "$b/miniaudio_split.o"
cc "$b/mix_test.o" "$b/miniaudio_split.o" -lm -lpthread -o "$b/mix_split"
"$b/mix_split"

echo
echo "== UBSan pass (single-header): any signed integer overflow aborts the run =="
# Scope the sanitizer to signed overflow, which is the guarantee the fixed-point accumulators make.
# (The library's PCM conversion code contains signed left shifts, a separate pre-existing pattern.)
cc $CFLAGS -fsanitize=signed-integer-overflow -fno-sanitize-recover=all -I. checks/mix_precision.c -lm -lpthread -o "$b/mix_ubsan"
"$b/mix_ubsan" >/dev/null
echo "UBSan: no signed integer overflow detected"
