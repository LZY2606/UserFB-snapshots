# Morph target submission

This note describes how the renderer submits morph targets (blend shapes) to
the GPU, how the fixed shader slot limit is respected, and how the submitted
data is refreshed and reused.

## Shader slot limit

The vertex shaders keep their original deformation formula and a fixed number
of attribute slots (see `src/shaders/shaderChunk/common_vert.glsl`):

- without morph normals: 8 `morphTarget0..7` position slots,
- with morph normals (`geometry.morphAttributes.normal` set): 4
  `morphTarget0..3` / `morphNormal0..3` position/normal pairs.

`transformed` accumulates `morphTargetK * morphTargetInfluences[K]` (and
`objectNormal` accumulates `morphNormalK * morphTargetInfluences[K]`), so the
uniform array has 8 entries without morph normals and 4 with them. The
renderer always submits the matching length.

## Weight semantics

Every non-zero entry of `mesh.morphTargetInfluences` is an active target.
Weights are used as-is: any finite positive or negative value is submitted
unchanged — they are never truncated, renormalized, or reduced to the
strongest few. This matches `Mesh#getVertexPosition`, so GPU rendering and
CPU queries stay consistent.

## How the slots are used

`WebGLRenderer._updateMorphtargets` (see `src/webgl/WebGLMorphtargets.js`)
runs before every draw of a mesh that has `morphTargetInfluences`:

- **Fits the slots** (`active <= 8`, or `active <= 4` with morph normals):
  the original target `Attribute`s are bound directly as
  `morphTarget0..k` / `morphNormal0..k` and the weights are copied into the
  uniform unchanged. No vertex data is copied, and interleaved source buffers
  keep working through `Buffer.stride` / `Attribute.offset`.
- **Overflows the slots**: all active targets are composited on the CPU into
  temporary slot buffers — active target `k` is folded into slot
  `k % slots` — and every slot is submitted with influence `1`, so the
  unmodified shader formula still accumulates every contribution. Only the 3
  components addressed by `Buffer.stride` / `Attribute.offset` are read;
  other interleaved components never enter the deformation. The temporary
  buffers are owned by the renderer-side morph state: the base
  `a_Position` / `a_Normal`, the source `morphAttributes` and the caller's
  weight array are never written to, and composited results are never shared
  between meshes that use different weights.

## Refresh and reuse of composited data

Temporary slot buffers are created lazily on the first overflow and reused
across frames (`BUFFER_USAGE.DYNAMIC_DRAW`). A composite is recomputed only
when its inputs change:

- any weight of the calling mesh changes,
- a target `Attribute` is replaced or a target is added,
- a source `Buffer.version` changes (bump `buffer.version` after editing a
  source array in place),
- the vertex count or the morph-normal configuration changes.

If none of these changed, the previous composite is reused as-is — no
recompute and no re-upload — which keeps alternating between characters that
share a geometry cheap. Uploads and releases go through the regular
`WebGLGeometries` / `WebGLBuffers` / `WebGLVertexArrayBindings` chain, and
`Geometry#dispose()` releases the temporary buffers like any other geometry
buffer.

## Regression test

`tests/morph-submit.js` exercises the real pre-draw path
(`WebGLGeometries.setGeometry` → `WebGLRenderer._updateMorphtargets` →
`WebGLVertexArrayBindings.setup`) against a lightweight device double that
records uniforms, attribute bindings and buffer uploads. It rebuilds the
shader-slot positions and normals by independent summation and compares them
with `Mesh#getVertexPosition` and analytic expectations (tolerance 1e-6),
covering 0/1/4/5/8/9 active targets, negative weights, interleaved buffers
with offsets, source buffer updates, shared-geometry A/B/A rendering, slot
transitions and resource release.

Run it with:

```sh
sh baseline/run-node.sh tests/morph-submit.js
# prints MORPH_SUBMIT_OK on success
```
