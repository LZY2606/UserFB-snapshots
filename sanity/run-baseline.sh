#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc -std=c99 -O1 -Iinclude -Isrc -Isrc/lib sanity/baseline.c \
 src/lib/datatypes/camera.c src/lib/datatypes/spline.c \
 src/common/transforms.c src/common/quaternion.c src/common/cr_assert.c \
 src/lib/renderer/samplers/sampler.c src/lib/renderer/samplers/random.c \
 src/lib/renderer/samplers/halton.c src/lib/renderer/samplers/hammersley.c \
 src/lib/vendored/pcg_basic.c -lm -o "$build/baseline"
"$build/baseline"
