#!/bin/sh
# Numeric checks for the pbr_surface tangent-space normal mapping fix.
#
# Compiles checks/check_uv_frame.cpp (plus the pbr_surface glTF loader) in
# a temporary directory, generates a small glTF asset with two triangles
# of opposite UV orientation and a constant normal map, then runs the
# checks. Also compiles and runs checks/baseline.cpp to make sure the
# legacy no-tangent-frame fallback is unchanged.
#
# Exits non-zero when any numeric assertion or face attribution fails.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-c++}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/uv_frame_check.XXXXXX")"
echo "check_uv_frame: working in $WORK"

INCLUDES="-I$REPO_ROOT -I$REPO_ROOT/examples/pbr_surface -I$REPO_ROOT/examples/common -I$REPO_ROOT/examples/common/glm/glm"

# --- generate the test glTF ---------------------------------------------
# Two XY-plane triangles sharing a constant (255,128,255,255) normal map:
#   face 0 (left):  UVs (0,0) (1,0) (0,1)  -> tilt towards +X
#   face 1 (right): UVs (0,0) (0,1) (1,0)  -> mirrored, tilt towards +Y
# The buffer holds 6 float3 positions, 6 float3 normals, 6 float2 UVs and
# 6 uint16 indices; the image is a 1x1 RGBA PNG.
cat > "$WORK/uv_test.gltf" <<'EOF'
{
  "asset": { "version": "2.0", "generator": "check_uv_frame.sh" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [
    {
      "primitives": [
        {
          "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 },
          "indices": 3,
          "material": 0,
          "mode": 4
        }
      ]
    }
  ],
  "materials": [
    {
      "pbrMetallicRoughness": {
        "baseColorFactor": [1, 1, 1, 1],
        "metallicFactor": 0.0,
        "roughnessFactor": 1.0
      },
      "normalTexture": { "index": 0 }
    }
  ],
  "textures": [ { "source": 0 } ],
  "images": [
    {
      "mimeType": "image/png",
      "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP43/D/PwAIfgN+VfwVfQAAAABJRU5ErkJggg=="
    }
  ],
  "buffers": [
    {
      "byteLength": 204,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAQAAAAAAAAAAAAABAQAAAAAAAAAAAAAAAQAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAgD8AAAAAAAABAAIAAwAEAAUA"
    }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 72 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 72 },
    { "buffer": 0, "byteOffset": 144, "byteLength": 48 },
    { "buffer": 0, "byteOffset": 192, "byteLength": 12 }
  ],
  "accessors": [
    {
      "bufferView": 0,
      "componentType": 5126,
      "count": 6,
      "type": "VEC3",
      "min": [0, 0, 0],
      "max": [3, 1, 0]
    },
    { "bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 6, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR" }
  ]
}
EOF

# --- compile -------------------------------------------------------------
# C++17: the bundled json.hpp only guards its std::string_view conversion
# behind JSON_HAS_CPP_17, and modern libc++ otherwise miscompiles
# tiny_gltf's string assignments.
echo "check_uv_frame: compiling"
# STB_IMAGE_IMPLEMENTATION: main.cc normally provides the stb_image
# implementation for the glTF loader; the check binary needs its own.
"$CXX" -std=c++17 -O1 -DSTB_IMAGE_IMPLEMENTATION $INCLUDES \
  "$REPO_ROOT/checks/check_uv_frame.cpp" \
  "$REPO_ROOT/examples/pbr_surface/gltf-loader.cc" \
  -o "$WORK/check_uv_frame"

"$CXX" -std=c++17 -O1 $INCLUDES \
  "$REPO_ROOT/checks/baseline.cpp" \
  -o "$WORK/baseline"

# --- run -----------------------------------------------------------------
echo "check_uv_frame: running"
"$WORK/check_uv_frame" "$WORK/uv_test.gltf" "$WORK/uv_frame_normals.ppm"

echo "check_uv_frame: running baseline (legacy fallback must be unchanged)"
"$WORK/baseline"

echo "check_uv_frame: OK (artifacts in $WORK)"
