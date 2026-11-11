#!/bin/sh
#
# Copyright (c), Recep Aslantas.
#
# MIT License (MIT), http://opensource.org/licenses/MIT
# Full license can be found in the LICENSE file
#

# Builds and runs the headless oblique near-plane tests with the system C
# compiler. No windowing system, test framework or installed cglm is needed;
# the in-tree headers are used directly. Returns 0 only when every check
# passes.

set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=c11 -O2 -Wall -Wextra"}
LDLIBS=${LDLIBS:-"-lm"}

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cglm-oblique.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT INT TERM

BIN="$WORK_DIR/oblique_test"

echo "compiling oblique tests with $CC"
$CC $CFLAGS \
    -I"$ROOT_DIR/include" \
    "$SCRIPT_DIR/oblique_test.c" \
    $LDLIBS \
    -o "$BIN"

"$BIN"
