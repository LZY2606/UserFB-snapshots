#!/bin/sh
# Builds and runs the checks for anisotropic GGX metal reflection in the
# CPU path tracer. Everything is built in a temporary directory; no window,
# external assets or denoiser are needed. Exits non-zero on failure.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-c++}"

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/yocto_anisotropic_metal.XXXXXX")"
echo "building in $BUILD_DIR"

case "$(uname)" in
  Linux) THREADS="-pthread" ;;
  *)     THREADS="" ;;
esac

# yocto library sources needed for CPU tracing and scene IO
YOCTO_SOURCES="
  $REPO/libs/yocto/yocto_bvh.cpp
  $REPO/libs/yocto/yocto_shape.cpp
  $REPO/libs/yocto/yocto_image.cpp
  $REPO/libs/yocto/yocto_scene.cpp
  $REPO/libs/yocto/yocto_trace.cpp
  $REPO/libs/yocto/yocto_modelio.cpp
  $REPO/libs/yocto/yocto_pbrtio.cpp
  $REPO/libs/yocto/yocto_sceneio.cpp
"
EXT_SOURCES="
  $REPO/exts/cgltf/cgltf/cgltf.c
  $REPO/exts/cgltf/cgltf/cgltf_write.c
  $REPO/exts/stb_image/stb_image/stb_image.cpp
  $REPO/exts/tinyexr/tinyexr/tinyexr.cpp
"

"$CXX" -std=c++17 -O2 \
  -I"$REPO/libs" \
  -I"$REPO/exts/cgltf" \
  -I"$REPO/exts/stb_image" \
  -I"$REPO/exts/tinyexr" \
  -I"$REPO/exts/nlohmann_json/single_include" \
  -I"$REPO/exts/fast_float/include" \
  "$REPO/regress/check_anisotropic_metal.cpp" \
  $YOCTO_SOURCES $EXT_SOURCES \
  $THREADS \
  -o "$BUILD_DIR/check_anisotropic_metal"

echo "running checks"
set +e
"$BUILD_DIR/check_anisotropic_metal" "$BUILD_DIR"
STATUS=$?
set -e

echo "artifacts (PPM renders, JSON scenes) in $BUILD_DIR"
if [ $STATUS -ne 0 ]; then
  echo "anisotropic metal checks FAILED"
  exit $STATUS
fi
echo "anisotropic metal checks passed"
