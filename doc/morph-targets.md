# Morph targets through world-space baking

Assimp stores per-vertex morph targets (blend shapes) as `aiMesh::mAnimMeshes`.
Historically `aiProcess_PreTransformVertices` dropped them when baking node
transforms into the mesh: with the hierarchy kept the base mesh moved but the
targets stayed in local space, and with the hierarchy collapsed they were
removed entirely.

This repository adds an opt-in mode that keeps them in both cases.

## Configuration

| Property | Value | Default |
| --- | --- | --- |
| `AI_CONFIG_PP_PTV_PRESERVE_MORPHS` (`"PP_PTV_PRESERVE_MORPHS"`) | bool | `false` |

```cpp
Assimp::Importer importer;
importer.SetPropertyBool(AI_CONFIG_PP_PTV_PRESERVE_MORPHS, true);
// optionally combined with AI_CONFIG_PP_PTV_KEEP_HIERARCHY /
// AI_CONFIG_PP_PTV_NORMALIZE
const aiScene *scene = importer.ReadFile(path,
        aiProcess_PreTransformVertices);
```

The option only affects the handling of `mAnimMeshes`. Animations are still
removed, and bones and node matrices keep the regular pre-transform handling.
When it is disabled the output is unchanged (targets are kept untouched in
keep-hierarchy mode and dropped when collapsing).

`Assimp::PretransformVertices::PreserveMorphs(bool)` offers the same toggle
when the step is driven directly.

## Baking rules

For every non-empty replacement stream of a target:

* positions use the full cumulative node matrix;
* normals, tangents and bitangents use the inverse-transpose of the base mesh
  and are normalized;
* UV and color channels are copied unchanged (they are not spatial data).

Empty arrays keep inheriting the host mesh, so an all-empty target stays a
valid no-op patch. Target names, weights, ordering and `aiMesh::mMethod` are
preserved, face winding and vertex correspondence stay aligned (a reflection
flips the faces only), and every output owns its buffers: destroying one
instance never affects another.

When the same source mesh is referenced by several nodes, each instance is
baked with its own cumulative matrix. The second bake therefore never sees an
already transformed source.

### Collapsing the hierarchy

Meshes are concatenated only when their material, base vertex format **and**
ordered target layout match. The layout identity is `mMethod`, the target
count, and every target's name and weight; which optional target channels are
present is not part of it. Meshes without targets never join meshes with
targets.

For compatible targets the present channels are unioned. A segment whose
input overrides a channel copies that value; a segment that does not falls
back to that instance's already baked base stream. If no input overrides a
channel at all the output array stays `nullptr`. Sparse UV/color slots are
preserved.

### Normalization

With `AI_CONFIG_PP_PTV_NORMALIZE` the translation and uniform scale are
derived from the bounding box of the **baked base** geometry only, and that
one mapping is then applied to every target position. Targets are allowed to
end up outside the base range; they are never normalized independently.

## Direct concatenation

`SceneCombiner::MergeMeshes` accepts the same contract via a new flag:

```cpp
#define AI_INT_MERGE_MESHES_PRESERVE_MORPHS 0x1

aiMesh *out = nullptr;
Assimp::SceneCombiner::MergeMeshes(
        &out, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
        meshes.begin(), meshes.end());
```

With the flag:

* inputs are not modified and their ownership stays with the caller (unlike
  `flags == 0`, which consumes them);
* incompatible ordered target layouts throw `std::invalid_argument`;
* an empty range still returns `nullptr`;
* channels are unioned exactly as described above.

`flags == 0` preserves the historical behaviour.
