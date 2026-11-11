#!/bin/sh
# Build the header-safety regression suite with ASan/UBSan in a temp
# directory and run it. Prints HEADER-SAFETY PASS on success; exits
# non-zero on any failure.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

CC=${CC:-cc}
b=$(mktemp -d "${TMPDIR:-/tmp}/header-safety.XXXXXX")
trap 'rm -rf "$b"' EXIT

SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

# The library must still compile standalone as ANSI C with no new
# runtime dependencies
$CC -std=c89 -pedantic -Wall -Wextra -Werror -Isrc -c src/microtar.c -o "$b/microtar.o"

$CC $SAN -Isrc tests/test_header_read.c  src/microtar.c -o "$b/test_header_read"
$CC $SAN -Isrc tests/test_header_write.c src/microtar.c -o "$b/test_header_write"
$CC $SAN -Isrc -DMTAR_FUZZ_STANDALONE tests/fuzz_header.c src/microtar.c -o "$b/fuzz_header"

export ASAN_OPTIONS=detect_leaks=0
"$b/test_header_read"
"$b/test_header_write"
"$b/fuzz_header"

echo "HEADER-SAFETY PASS"
