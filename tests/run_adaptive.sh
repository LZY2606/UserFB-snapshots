#!/bin/sh
# Build the adpcm-xq library plus the adaptive-encoder test harness and run it.
#
#   sh tests/run_adaptive.sh         normal build and run
#   sh tests/run_adaptive.sh asan    build with AddressSanitizer and run
#
# Exits non-zero if the build or any test fails.

set -e

cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="${CFLAGS:--O2} -std=c99 -Wall -Wextra -g"
LDFLAGS=""
MODE="normal"

if [ "$1" = "asan" ] || [ "$1" = "--asan" ]; then
    MODE="asan"
    CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
    LDFLAGS="-fsanitize=address,undefined"
fi

BIN="tests/test_adaptive${MODE:+_$MODE}"

echo "== building ($MODE) =="
# shellcheck disable=SC2086
$CC $CFLAGS -I. tests/test_adaptive.c adpcm-lib.c adpcm-dns.c -lm $LDFLAGS -o "$BIN"

echo "== running ($MODE) =="
"./$BIN"
