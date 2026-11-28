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
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file MorphTargetBake.h
 *  @brief Helpers for baking node transformations into morph targets
 *    (aiMesh::mAnimMeshes) and for stitching the morph targets of
 *    compatible meshes when meshes are merged.
 */
#pragma once
#ifndef AI_MORPHTARGETBAKE_H_INC
#define AI_MORPHTARGETBAKE_H_INC

#include <assimp/mesh.h>
#include <assimp/matrix4x4.h>

#include <vector>

namespace Assimp {

// ---------------------------------------------------------------------------
/** @brief Utilities used by PretransformVertices and SceneCombiner to keep
 *    morph targets in sync with the baked base meshes.
 *
 *  Anim mesh streams are absolute replacement streams, not offsets: a
 *  non-null array replaces the corresponding base mesh stream for the
 *  whole vertex range, while a null array inherits the base mesh.
 */
class ASSIMP_API MorphTargetBake {
public:
    MorphTargetBake() = delete;

    /** @brief One mesh contribution to a stitched morph target.
     *
     *  @c source provides the local-space anim meshes (and the morph
     *  layout), @c bakedBase provides the already transformed base
     *  streams which are used to fill segments whose anim mesh lacks a
     *  replacement channel. @c transform is the accumulated node matrix
     *  that was applied to the base mesh; a null pointer means identity
     *  (i.e. the base streams are already in their final space).
     */
    struct Segment {
        const aiMesh *source;
        const aiMatrix4x4 *transform;
        const aiMesh *bakedBase;
        unsigned int baseOffset;
        unsigned int outOffset;
        unsigned int vertexCount;
    };

    // -------------------------------------------------------------------
    /** @brief Apply an affine transformation to the spatial streams of
     *  a single anim mesh. Positions use the full matrix, normals,
     *  tangents and bitangents use the inverse-transpose of the linear
     *  part and are normalized. UV and color streams are not touched.
     *  Null (inherited) streams stay null.
     */
    static void TransformAnimMesh(aiAnimMesh &morph, const aiMatrix4x4 &mat);

    // -------------------------------------------------------------------
    /** @brief Transform every anim mesh attached to @c mesh. Used by the
     *  keep-hierarchy path of PretransformVertices.
     */
    static void TransformMeshMorphs(aiMesh &mesh, const aiMatrix4x4 &mat);

    // -------------------------------------------------------------------
    /** @brief Compare the ordered morph target layout of two meshes.
     *  The layout identity is mMethod, target count and, per target,
     *  name and weight. Which replacement channels are present is not
     *  part of the identity; a mesh without targets only matches
     *  another mesh without targets.
     */
    static bool SameMorphLayout(const aiMesh &a, const aiMesh &b);

    // -------------------------------------------------------------------
    /** @brief Stitch the anim meshes of the given segments.
     *
     *  All segments must share the same morph layout. For each target
     *  slot the union of replacement channels is built: segments with a
     *  replacement array contribute transformed target values, segments
     *  without one inherit the already transformed base mesh values of
     *  that instance. A channel missing in every input stays a null
     *  array. Target names, weights and order come from the first
     *  segment. The output arrays are owned independently and no input
     *  is modified.
     */
    static void BuildStitchedMorphs(const std::vector<Segment> &segments,
            aiAnimMesh **&out, unsigned int &numOut);
};

} // namespace Assimp

#endif // AI_MORPHTARGETBAKE_H_INC
