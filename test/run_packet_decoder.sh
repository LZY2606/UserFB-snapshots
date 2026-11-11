#! /bin/sh
# Build the bcg729 library and the packet decoder conformance test, then run
# it. The test is fully offline: it synthesises its own PCM, encodes speech
# frames with the library and builds SID frames from explicit bytes, so it
# needs no sound card, audio file or network access.
#
# Any pattern, sample or post-rejection state mismatch makes the test program
# exit non zero and this script propagates the failure.

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
TEST_SRC="$ROOT_DIR/test/src/packetDecoderTest.c"
TEST_BIN="$BUILD_DIR/packetDecoderTest"
TRACE_FILE="$BUILD_DIR/packet_trace.txt"

CC_BIN="${CC:-cc}"

echo "== Configuring bcg729 in $BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR"

echo "== Building bcg729 library"
cmake --build "$BUILD_DIR" --parallel

STATIC_LIB="$BUILD_DIR/src/libbcg729.a"
if [ ! -f "$STATIC_LIB" ]; then
	echo "error: $STATIC_LIB was not produced by the build" >&2
	exit 1
fi

echo "== Building packet decoder test program"
"$CC_BIN" -std=c99 -Wall -Wextra -I"$ROOT_DIR/include" \
	"$TEST_SRC" "$STATIC_LIB" -lm -o "$TEST_BIN"

echo "== Running packet decoder test, trace: $TRACE_FILE"
"$TEST_BIN" "$TRACE_FILE"
