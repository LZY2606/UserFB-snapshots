/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file MorphTargetUtils.h
 *  @brief Helpers shared by PretransformVertices and SceneCombiner for
 *         keeping aiMesh::mAnimMeshes alive while meshes are baked and
 *         concatenated.
 */
#ifndef AI_MORPH_TARGET_UTILS_H_INC
#define AI_MORPH_TARGET_UTILS_H_INC

#include <assimp/matrix3x3.h>
#include <assimp/matrix4x4.h>
#include <assimp/mesh.h>

#include <vector>

namespace Assimp {

// ---------------------------------------------------------------------------
/** Ordered morph target layout of a mesh.
 *
 *  Compatibility of meshes for concatenation is defined by this metadata:
 *  morphing method, target count, target names and target weights. Which
 *  optional vertex streams a target actually overrides is deliberately not
 *  part of the identity; streams are unioned when concatenating.
 */
struct MorphLayout {
    aiMorphingMethod mMethod = aiMorphingMethod_UNKNOWN;
    unsigned int mNumTargets = 0;
    std::vector<aiString> mNames;
    std::vector<float> mWeights;
};

// ---------------------------------------------------------------------------
/** Identity of an output bucket used while baking meshes in world space.
 *
 *  It combines the vertex data layout (which base vertex streams are stored,
 *  expressed as the historical GetMeshVFormatUnique() bit mask) with the
 *  ordered morph target layout. When morph targets are not preserved the
 *  morph part is simply empty.
 */
struct MorphTargetSpec {
    unsigned int mVertexFormat = 0;
    MorphLayout mLayout;
};

// ---------------------------------------------------------------------------
/** One occurrence of a source mesh in the scene graph, together with the
 *  cumulative transform it must be baked with. */
struct MorphSource {
    const aiMesh *mesh;
    aiMatrix4x4 transform;
};

// ---------------------------------------------------------------------------
/** Read the ordered morph target layout of a mesh. */
MorphLayout GetMorphLayout(const aiMesh *mesh);

// ---------------------------------------------------------------------------
/** Compare two layouts. */
bool MorphLayoutsEqual(const MorphLayout &a, const MorphLayout &b);

// ---------------------------------------------------------------------------
/** Check whether a mesh matches a previously read layout. */
bool MeshMatchesMorphLayout(const aiMesh *mesh, const MorphLayout &layout);

// ---------------------------------------------------------------------------
/** Bake the replacement streams of every morph target of @p mesh with
 *  @p mat. Only non-empty target arrays are touched: positions use the
 *  full matrix, normals/tangents/bitangents use the same inverse-transpose
 *  (plus normalization) rule as the base mesh, and UV/color channels are
 *  left alone because they are not spatial data. Empty arrays keep inheriting
 *  the host mesh. The call never reorders vertex data, so target vertices stay
 *  aligned with the base vertex streams.
 */
void BakeMorphTargets(aiMesh *mesh, const aiMatrix4x4 &mat);

// ---------------------------------------------------------------------------
/** Concatenate the morph targets of @p inputs into @p out.
 *
 *  The caller must already have concatenated the base vertex streams (and
 *  faces) of the same inputs into @p out, in the same order. Every input
 *  must share the same MorphLayout. For each target stream:
 *   - a segment whose input overrides the stream contributes that value;
 *   - a segment which inherits the stream is filled from the already
 *     concatenated base data of @p out;
 *   - if no input overrides the stream at all the output array stays null.
 *
 *  @param transforms Optional, one matrix per input. When given, target
 *         positions/normals/tangents/bitangents read from the input are baked
 *         with the corresponding matrix before they are stored. This is used
 *         by PretransformVertices, which must not modify the source meshes.
 *         When null the input target values are assumed to be final.
 */
void MergeMorphTargets(aiMesh *out,
        const std::vector<const aiMesh *> &inputs,
        const std::vector<aiMatrix4x4> *transforms = nullptr);

} // namespace Assimp

#endif // AI_MORPH_TARGET_UTILS_H_INC
