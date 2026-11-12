/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

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
---------------------------------------------------------------------------
*/

/** @file Implementation of the post processing step to join identical vertices
 * for all imported meshes
 */

#ifndef ASSIMP_BUILD_NO_JOINVERTICES_PROCESS

#include "JoinVerticesProcess.h"
#include "ProcessHelper.h"
#include <assimp/Vertex.h>
#include <assimp/TinyFormatter.h>

#include <stdio.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <map>

using namespace Assimp;

// ------------------------------------------------------------------------------------------------
// Returns whether the processing step is present in the given flag field.
bool JoinVerticesProcess::IsActive( unsigned int pFlags) const {
    return (pFlags & aiProcess_JoinIdenticalVertices) != 0;
}
// ------------------------------------------------------------------------------------------------
// Executes the post processing step on the given imported data.
void JoinVerticesProcess::Execute( aiScene* pScene) {
    ASSIMP_LOG_DEBUG("JoinVerticesProcess begin");

    // get the total number of vertices BEFORE the step is executed
    int iNumOldVertices = 0;
    if (!DefaultLogger::isNullLogger()) {
        for( unsigned int a = 0; a < pScene->mNumMeshes; a++)   {
            iNumOldVertices +=  pScene->mMeshes[a]->mNumVertices;
        }
    }

    // execute the step
    int iNumVertices = 0;
    for( unsigned int a = 0; a < pScene->mNumMeshes; a++) {
        iNumVertices += ProcessMesh( pScene->mMeshes[a],a);
    }

    pScene->mFlags |= AI_SCENE_FLAGS_NON_VERBOSE_FORMAT;

    // if logging is active, print detailed statistics
    if (!DefaultLogger::isNullLogger()) {
        if (iNumOldVertices == iNumVertices) {
            ASSIMP_LOG_DEBUG("JoinVerticesProcess finished ");
            return;
        }

        // Show statistics
        ASSIMP_LOG_INFO("JoinVerticesProcess finished | Verts in: ", iNumOldVertices,
            " out: ", iNumVertices, " | ~",
            ((iNumOldVertices - iNumVertices) / (float)iNumOldVertices) * 100.f );
    }
}

namespace {

/// Distance tolerance used for approximate attribute comparison.
static constexpr ai_real WELD_EPSILON = static_cast<ai_real>(1e-5);

/// Size of the spatial hash buckets used for position lookups. It is a multiple
/// of the tolerance: any two positions within WELD_EPSILON can then differ by at
/// most one bucket per axis, with a safety margin against floating-point
/// rounding in the bucket quantization.
static constexpr ai_real WELD_CELL_SIZE = static_cast<ai_real>(4) * WELD_EPSILON;

/// Compares two interleaved vertices using the historical distance tolerance.
/// Attributes that are not present in the mesh are zero-initialized by the
/// Vertex constructor, so they compare equal and are effectively ignored.
struct CompareVerticesAlmostEqual {
    bool operator () (const Vertex & a, const Vertex & b) const {
        static const ai_real squareEpsilon = WELD_EPSILON * WELD_EPSILON;

        // Non-finite positions cannot be compared by distance (NaN compares
        // false against everything, and would otherwise pass all checks).
        if (!std::isfinite(a.position.x) || !std::isfinite(a.position.y) ||
                !std::isfinite(a.position.z) || !std::isfinite(b.position.x) ||
                !std::isfinite(b.position.y) || !std::isfinite(b.position.z)) {
            return false;
        }

        if ((a.position - b.position).SquareLength() > squareEpsilon) {
            return false;
        }

        if ((a.normal - b.normal).SquareLength() > squareEpsilon) {
            return false;
        }

        if ((a.tangent - b.tangent).SquareLength() > squareEpsilon) {
            return false;
        }

        if ((a.bitangent - b.bitangent).SquareLength() > squareEpsilon) {
            return false;
        }

        for (uint32_t i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; i ++) {
            if ((a.texcoords[i] - b.texcoords[i]).SquareLength() > squareEpsilon) {
                return false;
            }
        }

        for (uint32_t i = 0; i < AI_MAX_NUMBER_OF_COLOR_SETS; i ++) {
            if (GetColorDifference(a.colors[i], b.colors[i]) > squareEpsilon) {
                return false;
            }
        }

        // If reached this point, they are ~equal
        return true;
    }
};

// ------------------------------------------------------------------------------------------------
// Sparse per-vertex bone signature: one entry per bone slot carrying a non-zero
// weight. Zero weights and missing entries are equivalent, the order of the
// entries does not matter, and weight values must match exactly. Bone slots
// stay independent identities; equal names do not make two bones the same.
struct BoneWeightEntry {
    unsigned int bone;
    ai_real weight;
};

using BoneSignature = std::vector<BoneWeightEntry>;

inline bool BoneSignaturesEqual(const BoneSignature &a, const BoneSignature &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].bone != b[i].bone || a[i].weight != b[i].weight) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Builds a normalized (sorted by bone slot, then weight) signature for every
// vertex of the mesh.
void BuildBoneSignatures(const aiMesh *mesh, std::vector<BoneSignature> &signatures) {
    signatures.assign(mesh->mNumVertices, BoneSignature());
    if (mesh->mNumBones == 0 || mesh->mBones == nullptr) {
        return;
    }

    for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
        const aiBone *bone = mesh->mBones[boneIndex];
        if (bone == nullptr || bone->mWeights == nullptr) {
            continue;
        }
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight &weight = bone->mWeights[w];
            if (weight.mVertexId >= mesh->mNumVertices) {
                continue;
            }
            // A zero weight is equivalent to the vertex not being influenced
            // by this bone at all.
            if (weight.mWeight == static_cast<ai_real>(0)) {
                continue;
            }
            signatures[weight.mVertexId].push_back({boneIndex, weight.mWeight});
        }
    }

    for (BoneSignature &signature : signatures) {
        std::sort(signature.begin(), signature.end(),
                [](const BoneWeightEntry &lhs, const BoneWeightEntry &rhs) {
                    if (lhs.bone != rhs.bone) {
                        return lhs.bone < rhs.bone;
                    }
                    return lhs.weight < rhs.weight;
                });
    }
}

// ------------------------------------------------------------------------------------------------
// Returns whether an animation target stores at least one vertex array.
bool AnimMeshHasArrays(const aiAnimMesh *anim) {
    if (anim->mVertices != nullptr || anim->mNormals != nullptr ||
            anim->mTangents != nullptr || anim->mBitangents != nullptr) {
        return true;
    }
    for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i) {
        if (anim->mTextureCoords[i] != nullptr) {
            return true;
        }
    }
    for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_COLOR_SETS; ++i) {
        if (anim->mColors[i] != nullptr) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
// Compares all animation targets at two vertex indices. Data streams that are
// actually stored in a target have to match exactly; streams that are absent
// inherit the base mesh data, which has already been compared (approximately)
// by CompareVerticesAlmostEqual. The current base pose alone is not enough:
// morph targets are their own vertex identity.
bool AnimStreamsEqual(const aiMesh *mesh, unsigned int a, unsigned int b) {
    for (unsigned int animIndex = 0; animIndex < mesh->mNumAnimMeshes; ++animIndex) {
        const aiAnimMesh *anim = mesh->mAnimMeshes[animIndex];
        if (anim == nullptr || anim->mNumVertices == 0) {
            // Vertex counts were validated up front: a target without arrays
            // inherits the base data and has nothing to compare.
            continue;
        }
        if (a >= anim->mNumVertices || b >= anim->mNumVertices) {
            return false;
        }
        if (anim->mVertices != nullptr && anim->mVertices[a] != anim->mVertices[b]) {
            return false;
        }
        if (anim->mNormals != nullptr && anim->mNormals[a] != anim->mNormals[b]) {
            return false;
        }
        // Tangents and bitangents are checked independently: aiAnimMesh only
        // reports the pair as present when mTangents is set.
        if (anim->mTangents != nullptr && anim->mTangents[a] != anim->mTangents[b]) {
            return false;
        }
        if (anim->mBitangents != nullptr && anim->mBitangents[a] != anim->mBitangents[b]) {
            return false;
        }
        for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i) {
            if (anim->mTextureCoords[i] != nullptr &&
                    anim->mTextureCoords[i][a] != anim->mTextureCoords[i][b]) {
                return false;
            }
        }
        for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_COLOR_SETS; ++i) {
            if (anim->mColors[i] != nullptr && anim->mColors[i][a] != anim->mColors[i][b]) {
                return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Spatial bucket key for approximate position lookups. Positions are
// quantized with a bucket size larger than the tolerance, so every position
// within the tolerance can only land in the same or an adjacent bucket.
// Coordinates are clamped to a safe integer range instead of overflowing for
// very large (finite) values; clamping only groups far-away positions
// together, which the distance test rejects afterwards.
struct WeldCellKey {
    int64_t x, y, z;

    bool operator==(const WeldCellKey &other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

inline int64_t WeldCellCoordinate(ai_real value) {
    // Keep well inside int64_t so that neighbouring cell arithmetic cannot
    // overflow either.
    static constexpr double kSafeLimit = 9.0e18;
    const double scaled = static_cast<double>(value) / static_cast<double>(WELD_CELL_SIZE);
    if (!std::isfinite(scaled)) {
        // Only reachable for non-finite input; keep such vertices in one
        // bucket so that the regular comparison rejects them.
        return 0;
    }
    if (scaled <= -kSafeLimit) {
        return static_cast<int64_t>(-kSafeLimit);
    }
    if (scaled >= kSafeLimit) {
        return static_cast<int64_t>(kSafeLimit);
    }
    return static_cast<int64_t>(std::floor(scaled));
}

struct WeldCellKeyHash {
    size_t operator()(const WeldCellKey &key) const {
        // 64-bit mix of the three cell coordinates.
        std::hash<int64_t> hasher;
        size_t seed = hasher(key.x);
        seed ^= hasher(key.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= hasher(key.z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

template<class XMesh>
void updateXMeshVertices(XMesh *pMesh, std::vector<int> &uniqueVertices) {
    // Record stream presence before changing mNumVertices: for aiAnimMesh the
    // presence helpers take mNumVertices into account.
    unsigned int numColorSets = 0;
    while (numColorSets < AI_MAX_NUMBER_OF_COLOR_SETS && pMesh->HasVertexColors(numColorSets)) {
        ++numColorSets;
    }
    unsigned int numTexCoordSets = 0;
    while (numTexCoordSets < AI_MAX_NUMBER_OF_TEXTURECOORDS && pMesh->HasTextureCoords(numTexCoordSets)) {
        ++numTexCoordSets;
    }

    // replace vertex data with the unique data sets
    pMesh->mNumVertices = (unsigned int)uniqueVertices.size();

    // ----------------------------------------------------------------------------
    // NOTE - we're *not* calling Vertex::SortBack() because it would check for
    // presence of every single vertex component once PER VERTEX. And our CPU
    // dislikes branches, even if they're easily predictable.
    // ----------------------------------------------------------------------------

    // Position, if present (check made for aiAnimMesh)
    if (pMesh->mVertices) {
        std::unique_ptr<aiVector3D[]> oldVertices(pMesh->mVertices);
        pMesh->mVertices = new aiVector3D[pMesh->mNumVertices];
        for (unsigned int a = 0; a < pMesh->mNumVertices; a++)
            pMesh->mVertices[a] = oldVertices[uniqueVertices[a]];
    }

    // Normals, if present
    if (pMesh->mNormals) {
        std::unique_ptr<aiVector3D[]> oldNormals(pMesh->mNormals);
        pMesh->mNormals = new aiVector3D[pMesh->mNumVertices];
        for (unsigned int a = 0; a < pMesh->mNumVertices; a++)
            pMesh->mNormals[a] = oldNormals[uniqueVertices[a]];
    }
    // Tangents, if present
    if (pMesh->mTangents) {
        std::unique_ptr<aiVector3D[]> oldTangents(pMesh->mTangents);
        pMesh->mTangents = new aiVector3D[pMesh->mNumVertices];
        for (unsigned int a = 0; a < pMesh->mNumVertices; a++)
            pMesh->mTangents[a] = oldTangents[uniqueVertices[a]];
    }
    // Bitangents as well
    if (pMesh->mBitangents) {
        std::unique_ptr<aiVector3D[]> oldBitangents(pMesh->mBitangents);
        pMesh->mBitangents = new aiVector3D[pMesh->mNumVertices];
        for (unsigned int a = 0; a < pMesh->mNumVertices; a++)
            pMesh->mBitangents[a] = oldBitangents[uniqueVertices[a]];
    }
    // Vertex colors
    for (unsigned int a = 0; a < numColorSets; a++) {
        std::unique_ptr<aiColor4D[]> oldColors(pMesh->mColors[a]);
        pMesh->mColors[a] = new aiColor4D[pMesh->mNumVertices];
        for (unsigned int b = 0; b < pMesh->mNumVertices; b++)
            pMesh->mColors[a][b] = oldColors[uniqueVertices[b]];
    }
    // Texture coords
    for (unsigned int a = 0; a < numTexCoordSets; a++) {
        std::unique_ptr<aiVector3D[]> oldTextureCoords(pMesh->mTextureCoords[a]);
        pMesh->mTextureCoords[a] = new aiVector3D[pMesh->mNumVertices];
        for (unsigned int b = 0; b < pMesh->mNumVertices; b++)
            pMesh->mTextureCoords[a][b] = oldTextureCoords[uniqueVertices[b]];
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------

static constexpr size_t JOINED_VERTICES_MARK = 0x80000000u;

// now start the JoinVerticesProcess
int JoinVerticesProcess::ProcessMesh( aiMesh* pMesh, unsigned int meshIndex) {
    static_assert( AI_MAX_NUMBER_OF_COLOR_SETS    == 8, "AI_MAX_NUMBER_OF_COLOR_SETS    == 8");
	static_assert( AI_MAX_NUMBER_OF_TEXTURECOORDS == 8, "AI_MAX_NUMBER_OF_TEXTURECOORDS == 8");

    // Return early if we don't have any positions
    if (!pMesh->HasPositions() || !pMesh->HasFaces()) {
        return 0;
    }

    // We should care only about used vertices, not all of them
    // (this can happen due to original file vertices buffer being used by
    // multiple meshes)
    std::vector<bool> usedVertexIndicesMask;
    usedVertexIndicesMask.resize(pMesh->mNumVertices, false);
    for (unsigned int a = 0; a < pMesh->mNumFaces; a++) {
        aiFace& face = pMesh->mFaces[a];
        for (unsigned int b = 0; b < face.mNumIndices; b++) {
            if (face.mIndices[b] >= pMesh->mNumVertices) {
                // Malformed face index: cannot remap safely, leave the mesh alone.
                ASSIMP_LOG_ERROR("JoinVerticesProcess: invalid face index in mesh ", meshIndex);
                return pMesh->mNumVertices;
            }
            usedVertexIndicesMask[face.mIndices[b]] = true;
        }
    }

    // We'll never have more vertices afterwards.
    std::vector<int> uniqueVertices;

    // For each vertex the index of the vertex it was replaced by.
    // Since the maximal number of vertices is 2^31-1, the most significand bit can be used to mark
    //  whether a new vertex was created for the index (true) or if it was replaced by an existing
    //  unique vertex (false). This saves an additional std::vector<bool> and greatly enhances
    //  branching performance.
    static_assert(AI_MAX_VERTICES == 0x7fffffff, "AI_MAX_VERTICES == 0x7fffffff");
    std::vector<unsigned int> replaceIndex( pMesh->mNumVertices, 0xffffffff);

    const bool hasAnimMeshes = pMesh->mNumAnimMeshes > 0;

    // Animation targets are indexed by the same vertex ids as the base mesh.
    // A target with a different vertex count cannot be remapped consistently,
    // so that (malformed) mesh is left untouched instead of producing
    // inconsistent indices.
    if (hasAnimMeshes) {
        for (unsigned int animMeshIndex = 0; animMeshIndex < pMesh->mNumAnimMeshes; ++animMeshIndex) {
            const aiAnimMesh *anim = pMesh->mAnimMeshes[animMeshIndex];
            if (anim == nullptr) {
                continue;
            }
            if (anim->mNumVertices != 0 && anim->mNumVertices != pMesh->mNumVertices) {
                ASSIMP_LOG_WARN("JoinVerticesProcess: skipping mesh ", meshIndex,
                        " because its animation target has a different vertex count");
                return pMesh->mNumVertices;
            }
            // Arrays without a usable vertex count cannot be compared or
            // remapped consistently, so leave the mesh untouched.
            if (anim->mNumVertices == 0 && AnimMeshHasArrays(anim)) {
                ASSIMP_LOG_WARN("JoinVerticesProcess: skipping mesh ", meshIndex,
                        " because its animation target stores arrays without a vertex count");
                return pMesh->mNumVertices;
            }
        }
    }

    // Bone weights are part of the vertex identity: build the sparse signature
    // of every vertex once, up front.
    std::vector<BoneSignature> boneSignatures;
    BuildBoneSignatures(pMesh, boneSignatures);

    // Spatial hash of already retained representatives, keyed by position cell.
    // Approximate equality is not transitive, so a vertex may only join a
    // representative that has already been retained and passes every test.
    // Scanning in original order and keeping the smallest matching original
    // index implements the "earliest representative" rule.
    std::unordered_map<WeldCellKey, std::vector<unsigned int>, WeldCellKeyHash> cells;

    // we can not end up with more vertices than we started with
    // Now check each vertex if it brings something new to the table
    int newIndex = 0;
    CompareVerticesAlmostEqual compareVertices;
    for( unsigned int a = 0; a < pMesh->mNumVertices; a++)  {
        // if the vertex is unused Do nothing
        if (!usedVertexIndicesMask[a]) {
            continue;
        }
        // collect the vertex data
        Vertex v(pMesh,a);

        const WeldCellKey cell = { WeldCellCoordinate(v.position.x),
                                   WeldCellCoordinate(v.position.y),
                                   WeldCellCoordinate(v.position.z) };

        // Search the cell itself plus all 26 neighbouring cells. The cell size
        // is larger than the tolerance, so two positions within tolerance can
        // only differ by at most one cell per axis. The sentinel is larger
        // than any possible original vertex index.
        unsigned int bestRepresentative = 0xffffffffu;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const WeldCellKey neighbour = { cell.x + dx, cell.y + dy, cell.z + dz };
                    auto cellIt = cells.find(neighbour);
                    if (cellIt == cells.end()) {
                        continue;
                    }
                    for (unsigned int representative : cellIt->second) {
                        if (representative >= bestRepresentative) {
                            // Representatives are visited in ascending original
                            // index order within a cell, but cells themselves
                            // are not ordered; skip anything that cannot win.
                            continue;
                        }
                        const Vertex other(pMesh, representative);
                        if (!compareVertices(v, other)) {
                            continue;
                        }
                        if (!BoneSignaturesEqual(boneSignatures[a], boneSignatures[representative])) {
                            continue;
                        }
                        if (!AnimStreamsEqual(pMesh, a, representative)) {
                            continue;
                        }
                        bestRepresentative = representative;
                    }
                }
            }
        }

        if (bestRepresentative != 0xffffffffu) {
            // join the earliest retained representative; keep the mark so bone
            // weights of joined vertices can be recognised.
            replaceIndex[a] = replaceIndex[bestRepresentative] | JOINED_VERTICES_MARK;
        } else {
            // this is a new vertex give it a new index
            replaceIndex[a] = newIndex++;
            // add the vertex to the unique vertices
            uniqueVertices.push_back(a);
            // register it as a representative in its spatial cell
            cells[cell].push_back(a);
        }
    }

    if (!DefaultLogger::isNullLogger() && DefaultLogger::get()->getLogSeverity() == Logger::VERBOSE)    {
        ASSIMP_LOG_VERBOSE_DEBUG(
            "Mesh ",meshIndex,
            " (",
            (pMesh->mName.length ? pMesh->mName.data : "unnamed"),
            ") | Verts in: ",pMesh->mNumVertices,
            " out: ",
            uniqueVertices.size(),
            " | ~",
            ((pMesh->mNumVertices - uniqueVertices.size()) / (float)pMesh->mNumVertices) * 100.f,
            "%"
        );
    }

    // The same original-to-new mapping is used for the base mesh and for every
    // animation target.
    updateXMeshVertices(pMesh, uniqueVertices);
    if (hasAnimMeshes) {
        for (unsigned int animMeshIndex = 0; animMeshIndex < pMesh->mNumAnimMeshes; animMeshIndex++) {
            updateXMeshVertices(pMesh->mAnimMeshes[animMeshIndex], uniqueVertices);
        }
    }

    // adjust the indices in all faces
    for( unsigned int a = 0; a < pMesh->mNumFaces; a++) {
        aiFace& face = pMesh->mFaces[a];
        for( unsigned int b = 0; b < face.mNumIndices; b++) {
            face.mIndices[b] = replaceIndex[face.mIndices[b]] & ~JOINED_VERTICES_MARK;
        }
    }

    // adjust bone vertex weights.
    for( int a = 0; a < (int)pMesh->mNumBones; a++) {
        aiBone* bone = pMesh->mBones[a];
        if (bone == nullptr) {
            continue;
        }
        std::vector<aiVertexWeight> newWeights;
        newWeights.reserve( bone->mNumWeights);

        if (nullptr != bone->mWeights) {
            std::unordered_set<unsigned int> visited;
            visited.reserve(bone->mNumWeights);
            for ( unsigned int b = 0; b < bone->mNumWeights; b++ ) {
                const aiVertexWeight& ow = bone->mWeights[ b ];
                // The ids in the bone records are original vertex ids, so the
                // mapping table (sized for the original vertex count) is the
                // correct bound here - pMesh->mNumVertices already holds the
                // reduced count at this point.
                if (ow.mVertexId >= replaceIndex.size()) {
                    continue;
                }
                // A zero weight is equivalent to the vertex not being
                // influenced by this bone; there is nothing left to record.
                if (ow.mWeight == static_cast<ai_real>(0)) {
                    continue;
                }
                // Vertices that are not referenced by any face are removed.
                const unsigned int mapped = replaceIndex[ow.mVertexId];
                if (mapped == 0xffffffffu) {
                    continue;
                }
                const unsigned int newVertexId = mapped & ~JOINED_VERTICES_MARK;
                // Joined vertices share their identity, so several old weights
                // may collapse onto the same new vertex id. Keep only one entry
                // per bone and new vertex.
                if (!visited.insert(newVertexId).second) {
                    continue;
                }
                aiVertexWeight nw;
                nw.mVertexId = newVertexId;
                nw.mWeight = ow.mWeight;
                newWeights.push_back( nw );
            }
        } else if (bone->mNumWeights > 0) {
            ASSIMP_LOG_ERROR( "X-Export: aiBone shall contain weights, but pointer to them is nullptr." );
        }

        // kill the old and replace them with the translated weights. A bone
        // without any remaining weights keeps its identity but gets an empty
        // weight list.
        delete [] bone->mWeights;
        bone->mWeights = nullptr;
        bone->mNumWeights = 0;
        if (!newWeights.empty()) {
            bone->mNumWeights = (unsigned int)newWeights.size();
            bone->mWeights = new aiVertexWeight[bone->mNumWeights];
            memcpy( bone->mWeights, &newWeights[0], bone->mNumWeights * sizeof( aiVertexWeight));
        }
    }
    return pMesh->mNumVertices;
}

#endif // !! ASSIMP_BUILD_NO_JOINVERTICES_PROCESS
