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

#include "MorphTargetUtils.h"

#include <cmath>

namespace Assimp {

namespace {

// ---------------------------------------------------------------------------
// Transform one vector stream of a morph target in place.
void BakeVectorStream(aiVector3D *values, unsigned int count, const aiMatrix3x3 &normalMatrix) {
    for (unsigned int i = 0; i < count; ++i) {
        values[i] = (normalMatrix * values[i]).Normalize();
    }
}

// ---------------------------------------------------------------------------
// Copy one vector stream segment. When @p src is present it is copied (and
// baked when a transform is given: positions use the full matrix, other
// vectors the inverse-transpose normal rule). When the target does not
// override the stream, the value is inherited from the already final base
// segment in @p base; that base data must not be transformed again.
void CopyVectorSegment(aiVector3D *dst, const aiVector3D *src,
        const aiVector3D *base, unsigned int count,
        const aiMatrix4x4 *transform, const aiMatrix3x3 *normalMatrix,
        bool positions) {
    if (src == nullptr) {
        if (base != nullptr) {
            for (unsigned int i = 0; i < count; ++i) {
                dst[i] = base[i];
            }
        }
        return; // otherwise leave the zero-initialized destination
    }
    if (transform == nullptr || transform->IsIdentity()) {
        // Matches the host-mesh code path, which copies untouched for an
        // identity node transform.
        for (unsigned int i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
    } else if (positions) {
        for (unsigned int i = 0; i < count; ++i) {
            dst[i] = (*transform) * src[i];
        }
    } else {
        for (unsigned int i = 0; i < count; ++i) {
            dst[i] = ((*normalMatrix) * src[i]).Normalize();
        }
    }
}

// ---------------------------------------------------------------------------
// Copy one colour stream segment. Targets can inherit the base value, in
// which case @p base is used; colours are never spatially transformed.
void CopyColorSegment(aiColor4D *dst, const aiColor4D *src, const aiColor4D *base, unsigned int count) {
    if (src == nullptr) {
        src = base;
    }
    if (src == nullptr) {
        return;
    }
    for (unsigned int i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

// ---------------------------------------------------------------------------
// Check whether any input target overrides a given vector stream.
template <typename Getter>
bool AnyTargetHas(unsigned int targetIndex, const std::vector<const aiMesh *> &inputs, Getter getter) {
    for (const aiMesh *mesh : inputs) {
        if (mesh == nullptr || mesh->mAnimMeshes == nullptr || targetIndex >= mesh->mNumAnimMeshes) {
            continue;
        }
        const aiAnimMesh *target = mesh->mAnimMeshes[targetIndex];
        if (target != nullptr && getter(target)) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
MorphLayout GetMorphLayout(const aiMesh *mesh) {
    MorphLayout layout;
    layout.mMethod = aiMorphingMethod_UNKNOWN;
    layout.mNumTargets = 0;
    if (mesh == nullptr) {
        return layout;
    }

    layout.mMethod = mesh->mMethod;
    layout.mNumTargets = mesh->mNumAnimMeshes;
    layout.mNames.reserve(mesh->mNumAnimMeshes);
    layout.mWeights.reserve(mesh->mNumAnimMeshes);
    if (mesh->mAnimMeshes == nullptr) {
        return layout;
    }
    for (unsigned int i = 0; i < mesh->mNumAnimMeshes; ++i) {
        const aiAnimMesh *target = mesh->mAnimMeshes[i];
        if (target != nullptr) {
            layout.mNames.push_back(target->mName);
            layout.mWeights.push_back(target->mWeight);
        } else {
            layout.mNames.emplace_back();
            layout.mWeights.push_back(0.0f);
        }
    }
    return layout;
}

// ---------------------------------------------------------------------------
bool MorphLayoutsEqual(const MorphLayout &a, const MorphLayout &b) {
    if (a.mMethod != b.mMethod || a.mNumTargets != b.mNumTargets) {
        return false;
    }
    if (a.mNames.size() != b.mNames.size() || a.mWeights.size() != b.mWeights.size()) {
        return false;
    }
    for (size_t i = 0; i < a.mNames.size(); ++i) {
        if (a.mNames[i] != b.mNames[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < a.mWeights.size(); ++i) {
        if (a.mWeights[i] != b.mWeights[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
bool MeshMatchesMorphLayout(const aiMesh *mesh, const MorphLayout &layout) {
    return MorphLayoutsEqual(GetMorphLayout(mesh), layout);
}

// ---------------------------------------------------------------------------
void BakeMorphTargets(aiMesh *mesh, const aiMatrix4x4 &mat) {
    if (mesh == nullptr || mesh->mAnimMeshes == nullptr || mat.IsIdentity()) {
        return;
    }

    const aiMatrix3x3 normalMatrix = aiMatrix3x3(mat).Inverse().Transpose();

    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        aiAnimMesh *target = mesh->mAnimMeshes[t];
        if (target == nullptr) {
            continue;
        }
        // A valid aiAnimMesh repeats the host count. Fall back to the host
        // count if an importer left it unset.
        const unsigned int count = target->mNumVertices ? target->mNumVertices : mesh->mNumVertices;

        if (target->mVertices != nullptr) {
            for (unsigned int i = 0; i < count; ++i) {
                target->mVertices[i] = mat * target->mVertices[i];
            }
        }
        if (target->mNormals != nullptr) {
            BakeVectorStream(target->mNormals, count, normalMatrix);
        }
        if (target->mTangents != nullptr) {
            BakeVectorStream(target->mTangents, count, normalMatrix);
        }
        if (target->mBitangents != nullptr) {
            BakeVectorStream(target->mBitangents, count, normalMatrix);
        }
        // Texture coordinates and colours are not spatial data and are
        // deliberately left untouched.
    }
}

// ---------------------------------------------------------------------------
void MergeMorphTargets(aiMesh *out,
        const std::vector<const aiMesh *> &inputs,
        const std::vector<aiMatrix4x4> *transforms) {
    if (out == nullptr || inputs.empty()) {
        return;
    }
    if (transforms != nullptr && transforms->size() != inputs.size()) {
        return;
    }

    const aiMesh *first = inputs.front();
    out->mMethod = (first != nullptr) ? first->mMethod : aiMorphingMethod_UNKNOWN;

    if (first == nullptr || first->mNumAnimMeshes == 0) {
        return;
    }

    const unsigned int numTargets = first->mNumAnimMeshes;
    const unsigned int numVertices = out->mNumVertices;

    out->mNumAnimMeshes = numTargets;
    out->mAnimMeshes = new aiAnimMesh *[numTargets];

    for (unsigned int t = 0; t < numTargets; ++t) {
        aiAnimMesh *target = out->mAnimMeshes[t] = new aiAnimMesh();
        target->mNumVertices = numVertices;
        if (first->mAnimMeshes != nullptr && first->mAnimMeshes[t] != nullptr) {
            target->mName = first->mAnimMeshes[t]->mName;
            target->mWeight = first->mAnimMeshes[t]->mWeight;
        }

        unsigned int offset = 0;
        if (AnyTargetHas(t, inputs, [](const aiAnimMesh *a) { return a->mVertices != nullptr; })) {
            target->mVertices = new aiVector3D[numVertices];
        }
        if (AnyTargetHas(t, inputs, [](const aiAnimMesh *a) { return a->mNormals != nullptr; })) {
            target->mNormals = new aiVector3D[numVertices];
        }
        if (AnyTargetHas(t, inputs, [](const aiAnimMesh *a) { return a->mTangents != nullptr; })) {
            target->mTangents = new aiVector3D[numVertices];
        }
        if (AnyTargetHas(t, inputs, [](const aiAnimMesh *a) { return a->mBitangents != nullptr; })) {
            target->mBitangents = new aiVector3D[numVertices];
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
            if (AnyTargetHas(t, inputs, [c](const aiAnimMesh *a) { return a->mTextureCoords[c] != nullptr; })) {
                target->mTextureCoords[c] = new aiVector3D[numVertices];
            }
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
            if (AnyTargetHas(t, inputs, [c](const aiAnimMesh *a) { return a->mColors[c] != nullptr; })) {
                target->mColors[c] = new aiColor4D[numVertices];
            }
        }

        offset = 0;
        for (unsigned int s = 0; s < inputs.size(); ++s) {
            const aiMesh *mesh = inputs[s];
            const unsigned int count = (mesh != nullptr) ? mesh->mNumVertices : 0;
            if (offset + count > numVertices) {
                break;
            }

            const aiAnimMesh *srcTarget = nullptr;
            if (mesh != nullptr && mesh->mAnimMeshes != nullptr && t < mesh->mNumAnimMeshes) {
                srcTarget = mesh->mAnimMeshes[t];
            }

            const aiMatrix4x4 *transform = nullptr;
            aiMatrix3x3 normalMatrix;
            if (transforms != nullptr && count) {
                transform = &(*transforms)[s];
                normalMatrix = aiMatrix3x3(*transform).Inverse().Transpose();
            }

            if (target->mVertices != nullptr) {
                CopyVectorSegment(target->mVertices + offset,
                        srcTarget ? srcTarget->mVertices : nullptr,
                        (out->mVertices != nullptr) ? out->mVertices + offset : nullptr,
                        count, transform, &normalMatrix, true);
            }
            if (target->mNormals != nullptr) {
                CopyVectorSegment(target->mNormals + offset,
                        srcTarget ? srcTarget->mNormals : nullptr,
                        (out->mNormals != nullptr) ? out->mNormals + offset : nullptr,
                        count, transform, &normalMatrix, false);
            }
            if (target->mTangents != nullptr) {
                CopyVectorSegment(target->mTangents + offset,
                        srcTarget ? srcTarget->mTangents : nullptr,
                        (out->mTangents != nullptr) ? out->mTangents + offset : nullptr,
                        count, transform, &normalMatrix, false);
            }
            if (target->mBitangents != nullptr) {
                CopyVectorSegment(target->mBitangents + offset,
                        srcTarget ? srcTarget->mBitangents : nullptr,
                        (out->mBitangents != nullptr) ? out->mBitangents + offset : nullptr,
                        count, transform, &normalMatrix, false);
            }
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
                if (target->mTextureCoords[c] != nullptr) {
                    const aiVector3D *base = (out->mTextureCoords[c] != nullptr)
                            ? out->mTextureCoords[c] + offset
                            : nullptr;
                    CopyVectorSegment(target->mTextureCoords[c] + offset,
                            srcTarget ? srcTarget->mTextureCoords[c] : nullptr,
                            base, count, nullptr, nullptr, false);
                }
            }
            for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
                if (target->mColors[c] != nullptr) {
                    CopyColorSegment(target->mColors[c] + offset,
                            srcTarget ? srcTarget->mColors[c] : nullptr,
                            (out->mColors[c] != nullptr) ? out->mColors[c] + offset : nullptr,
                            count);
                }
            }

            offset += count;
        }
    }
}

} // namespace Assimp
