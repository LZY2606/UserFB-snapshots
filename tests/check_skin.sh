#!/bin/sh
# Build and run the mathc_skin_vertex checks in a temporary directory, for
# both the default (float) and the MATHC_USE_DOUBLE_FLOATING_POINT configs.
# Run from the repository root: sh tests/check_skin.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/mathc-skin-check.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT

for cfg in float double; do
	if [ "$cfg" = "double" ]; then
		DEF=-DMATHC_USE_DOUBLE_FLOATING_POINT
	else
		DEF=
	fi
	cc -std=c99 -Wall -Wextra $DEF -I"$ROOT" \
		-c "$ROOT/tests/check_skin.c" -o "$BUILD/check_skin_$cfg.o"
	cc -std=c99 -Wall -Wextra $DEF -I"$ROOT" \
		-c "$ROOT/mathc_skin.c" -o "$BUILD/mathc_skin_$cfg.o"
	cc -std=c99 $DEF -I"$ROOT" \
		-c "$ROOT/mathc.c" -o "$BUILD/mathc_$cfg.o"
	cc $DEF "$BUILD/check_skin_$cfg.o" "$BUILD/mathc_skin_$cfg.o" \
		"$BUILD/mathc_$cfg.o" -lm -o "$BUILD/check_skin_$cfg"
	"$BUILD/check_skin_$cfg"
done

echo "check_skin: all configurations passed"
