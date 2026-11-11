#!/bin/sh
#
# headless regression for the tiled detail normal layer of the pbr shader.
# builds the renderer library (without any window platform code) together with
# regress/detail_normals.c in a temporary directory, generates small meshes,
# textures and scenes on the fly, and checks the composited normal math, the
# uv tiling/offset behavior, mirrored tangent handedness, backface flipping,
# the optional scene block and the shared texture cache. no window and no
# large model asset is required. set DETAIL_NORMAL_OUTPUT to keep the density
# comparison image somewhere other than regress/output.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
CC=${CC:-cc}
CFLAGS="-std=c89 -Wall -Wextra -pedantic -O3 -flto -ffast-math"
# mirror build_linux.sh: expose the C99 isfinite declaration under strict c89
case $(uname -s) in
    Darwin) ;;
    *) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L" ;;
esac
OUT_DIR=${DETAIL_NORMAL_OUTPUT:-"$SCRIPT_DIR/output"}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/detail_normals.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/build" "$WORK/run/detail" "$OUT_DIR"

CORE_SRCS="
    camera.c darray.c draw2d.c graphics.c image.c maths.c mesh.c
    private.c scene.c skeleton.c texture.c
"
SCENE_SRCS="blinn_scenes.c pbr_scenes.c scene_helper.c"
SHADER_SRCS="blinn_shader.c cache_helper.c pbr_shader.c skybox_shader.c"

build_sources() {
    for name in $CORE_SRCS; do
        printf '%s ' "$REPO/renderer/core/$name"
    done
    for name in $SCENE_SRCS; do
        printf '%s ' "$REPO/renderer/scenes/$name"
    done
    for name in $SHADER_SRCS; do
        printf '%s ' "$REPO/renderer/shaders/$name"
    done
    printf '%s ' "$SCRIPT_DIR/detail_normals.c"
}

# shellcheck disable=SC2046
$CC $CFLAGS -I"$REPO" $(build_sources) -lm \
    -o "$WORK/build/check_detail_normals"

cd "$WORK/run"
"$WORK/build/check_detail_normals" "$WORK/run"

cp "$WORK/run/detail_density.tga" "$OUT_DIR/detail_density.tga"
echo "density comparison image copied to: $OUT_DIR/detail_density.tga"
