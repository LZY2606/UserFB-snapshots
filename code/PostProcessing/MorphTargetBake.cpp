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

/// @file  MorphTargetBake.cpp
/// @brief Baking of node transformations into morph target anim meshes
///   and stitching of morph targets when compatible meshes are merged.

#include "MorphTargetBake.h"

#include <cstring>

namespace Assimp {

// ------------------------------------------------------------------------------------------------
// Local helper: transform a position array in place (full affine matrix).
static void BakePositions(aiVector3D *dst, const aiVector3D *src, unsigned int count,
        const aiMatrix4x4 *mat) {
    if (mat) {
        for (unsigned int i = 0; i < count; ++i) {
            dst[i] = (*mat) * src[i];
        }
    } else {
        ::memcpy(dst, src, count * sizeof(aiVector3D));
    }
}

// ------------------------------------------------------------------------------------------------
// Local helper: transform a normal/tangent/bitangent array in place using
// the inverse-transpose of the affine linear part, normalized per vector.
static void BakeDirections(aiVector3D *dst, const aiVector3D *src, unsigned int count,
        const aiMatrix4x4 *mat) {
    if (!mat) {
        ::memcpy(dst, src, count * sizeof(aiVector3D));
        return;
    }

    const aiMatrix3x3 normal = aiMatrix3x3(*mat).Inverse().Transpose();
    for (unsigned int i = 0; i < count; ++i) {
        dst[i] = (normal * src[i]).Normalize();
    }
}

// ------------------------------------------------------------------------------------------------
void MorphTargetBake::TransformAnimMesh(aiAnimMesh &morph, const aiMatrix4x4 &mat) {
    if (mat.IsIdentity()) {
        return;
    }

    const unsigned int count = morph.mNumVertices;
    if (morph.mVertices) {
        BakePositions(morph.mVertices, morph.mVertices, count, &mat);
    }
    if (morph.mNormals) {
        BakeDirections(morph.mNormals, morph.mNormals, count, &mat);
    }
    if (morph.mTangents) {
        BakeDirections(morph.mTangents, morph.mTangents, count, &mat);
    }
    if (morph.mBitangents) {
        BakeDirections(morph.mBitangents, morph.mBitangents, count, &mat);
    }
    // UV and color replacement streams are never spatially transformed.
}

// ------------------------------------------------------------------------------------------------
void MorphTargetBake::TransformMeshMorphs(aiMesh &mesh, const aiMatrix4x4 &mat) {
    for (unsigned int i = 0; i < mesh.mNumAnimMeshes; ++i) {
        TransformAnimMesh(*mesh.mAnimMeshes[i], mat);
    }
}

// ------------------------------------------------------------------------------------------------
bool MorphTargetBake::SameMorphLayout(const aiMesh &a, const aiMesh &b) {
    if (a.mMethod != b.mMethod || a.mNumAnimMeshes != b.mNumAnimMeshes) {
        return false;
    }

    for (unsigned int i = 0; i < a.mNumAnimMeshes; ++i) {
        const aiAnimMesh &x = *a.mAnimMeshes[i];
        const aiAnimMesh &y = *b.mAnimMeshes[i];
        if (x.mName != y.mName || x.mWeight != y.mWeight) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
void MorphTargetBake::BuildStitchedMorphs(const std::vector<Segment> &segments,
        aiAnimMesh **&out, unsigned int &numOut) {
    out = nullptr;
    numOut = 0;
    if (segments.empty()) {
        return;
    }

    const aiMesh *first = segments.front().source;
    numOut = first->mNumAnimMeshes;
    if (!numOut) {
        return;
    }

    unsigned int totalVertices = 0;
    for (const Segment &s : segments) {
        totalVertices += s.vertexCount;
    }

    out = new aiAnimMesh *[numOut];
    for (unsigned int t = 0; t < numOut; ++t) {
        const aiAnimMesh *firstTarget = first->mAnimMeshes[t];

        // Which replacement channels exist anywhere for this target slot?
        bool hasPositions = false, hasNormals = false;
        bool hasTangents = false, hasBitangents = false;
        bool hasUV[AI_MAX_NUMBER_OF_TEXTURECOORDS] = {};
        bool hasColor[AI_MAX_NUMBER_OF_COLOR_SETS] = {};
        for (const Segment &s : segments) {
            const aiAnimMesh *m = s.source->mAnimMeshes[t];
            hasPositions = hasPositions || m->mVertices != nullptr;
            hasNormals = hasNormals || m->mNormals != nullptr;
            hasTangents = hasTangents || m->mTangents != nullptr;
            hasBitangents = hasBitangents || m->mBitangents != nullptr;
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
                hasUV[c] = hasUV[c] || m->mTextureCoords[c] != nullptr;
            }
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
                hasColor[c] = hasColor[c] || m->mColors[c] != nullptr;
            }
        }

        aiAnimMesh *stitched = out[t] = new aiAnimMesh();
        stitched->mName = firstTarget->mName;
        stitched->mWeight = firstTarget->mWeight;
        stitched->mNumVertices = totalVertices;

        if (hasPositions) {
            stitched->mVertices = new aiVector3D[totalVertices];
        }
        if (hasNormals) {
            stitched->mNormals = new aiVector3D[totalVertices];
        }
        if (hasTangents) {
            stitched->mTangents = new aiVector3D[totalVertices];
        }
        if (hasBitangents) {
            stitched->mBitangents = new aiVector3D[totalVertices];
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
            if (hasUV[c]) {
                stitched->mTextureCoords[c] = new aiVector3D[totalVertices];
            }
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
            if (hasColor[c]) {
                stitched->mColors[c] = new aiColor4D[totalVertices];
            }
        }

        // Fill the segments. A replacement array contributes transformed
        // target values; a missing one inherits the baked base values of
        // the same instance.
        for (const Segment &s : segments) {
            const aiAnimMesh *m = s.source->mAnimMeshes[t];
            const unsigned int n = s.vertexCount;
            const unsigned int o = s.outOffset;
            const unsigned int b = s.baseOffset;

            if (stitched->mVertices) {
                if (m->mVertices) {
                    BakePositions(stitched->mVertices + o, m->mVertices, n, s.transform);
                } else {
                    ::memcpy(stitched->mVertices + o, s.bakedBase->mVertices + b,
                            n * sizeof(aiVector3D));
                }
            }
            if (stitched->mNormals) {
                if (m->mNormals) {
                    BakeDirections(stitched->mNormals + o, m->mNormals, n, s.transform);
                } else if (s.bakedBase->mNormals) {
                    ::memcpy(stitched->mNormals + o, s.bakedBase->mNormals + b,
                            n * sizeof(aiVector3D));
                }
            }
            if (stitched->mTangents) {
                if (m->mTangents) {
                    BakeDirections(stitched->mTangents + o, m->mTangents, n, s.transform);
                } else if (s.bakedBase->mTangents) {
                    ::memcpy(stitched->mTangents + o, s.bakedBase->mTangents + b,
                            n * sizeof(aiVector3D));
                }
            }
            if (stitched->mBitangents) {
                if (m->mBitangents) {
                    BakeDirections(stitched->mBitangents + o, m->mBitangents, n, s.transform);
                } else if (s.bakedBase->mBitangents) {
                    ::memcpy(stitched->mBitangents + o, s.bakedBase->mBitangents + b,
                            n * sizeof(aiVector3D));
                }
            }
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
                if (!stitched->mTextureCoords[c]) {
                    continue;
                }
                if (m->mTextureCoords[c]) {
                    // UVs are not spatially transformed.
                    ::memcpy(stitched->mTextureCoords[c] + o, m->mTextureCoords[c],
                            n * sizeof(aiVector3D));
                } else if (s.bakedBase->mTextureCoords[c]) {
                    ::memcpy(stitched->mTextureCoords[c] + o,
                            s.bakedBase->mTextureCoords[c] + b,
                            n * sizeof(aiVector3D));
                }
            }
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
                if (!stitched->mColors[c]) {
                    continue;
                }
                if (m->mColors[c]) {
                    // Colors are not spatially transformed.
                    ::memcpy(stitched->mColors[c] + o, m->mColors[c],
                            n * sizeof(aiColor4D));
                } else if (s.bakedBase->mColors[c]) {
                    ::memcpy(stitched->mColors[c] + o, s.bakedBase->mColors[c] + b,
                            n * sizeof(aiColor4D));
                }
            }
        }
    }
}

} // namespace Assimp
