// Regression tests for JoinVerticesProcess vertex identity and bone-aware welding.
//
// This file is included by harness/registered.cpp. It contains real assertions
// only (WELD_CHECK) and works on small in-memory meshes: no importer, no model
// files.
//
// Cases:
//   epsilon_candidates   - nearby positions across bucket borders (positive,
//                          negative), approximate attribute streams, and the
//                          non-transitive representative rule.
//   representative_chain - explicit A~B, B~C, A!~C chain, huge finite
//                          coordinates and clamping safety.
//   skin_signatures      - bone weight signatures as part of vertex identity,
//                          skinning preserved per face corner.
//   animated_attributes  - morph target streams must match exactly; missing
//                          arrays inherit the base data.
//   remap_and_legacy     - bone remapping / pruning, Execute() entry point and
//                          re-use across two meshes.

#include "../harness/test-harness.h"

#include <assimp/scene.h>
#include <assimp/matrix4x4.h>
#include "PostProcessing/JoinVerticesProcess.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace weld {

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

/// Declarative description of a small test mesh. Arrays that stay empty are
/// absent from the resulting mesh.
struct MeshSpec {
    struct WeightSpec {
        unsigned vertex;
        float weight;
    };

    struct BoneSpec {
        std::string name;
        aiMatrix4x4 offset;
        std::vector<WeightSpec> weights;
    };

    struct AnimSpec {
        std::string name;
        std::vector<aiVector3D> positions; // empty => array absent (uses base)
        std::vector<aiVector3D> normals;
        std::vector<aiVector3D> tangents;
        std::vector<aiVector3D> bitangents;
        std::vector<aiVector3D> texCoords;
        std::vector<aiColor4D> colors;
    };

    std::vector<aiVector3D> positions;
    std::vector<aiVector3D> normals;   // empty => absent
    std::vector<aiVector3D> texCoords;
    std::vector<aiColor4D> colors;
    std::vector<std::vector<unsigned>> faces;
    std::vector<BoneSpec> bones;
    std::vector<AnimSpec> anims;
    unsigned primitiveTypes = aiPrimitiveType_TRIANGLE;
    unsigned materialIndex = 0;
};

inline aiMatrix4x4 MakeOffset(float angle, float tx, float ty, float tz) {
    aiMatrix4x4 rotation;
    aiMatrix4x4::RotationX(angle, rotation);
    aiMatrix4x4 result = rotation;
    result.a4 = tx;
    result.b4 = ty;
    result.c4 = tz;
    return result;
}

/// Materializes a MeshSpec into a heap-allocated aiMesh (ownership transfer).
inline aiMesh *BuildMesh(const MeshSpec &spec) {
    aiMesh *mesh = new aiMesh();
    mesh->mPrimitiveTypes = spec.primitiveTypes;
    mesh->mMaterialIndex = spec.materialIndex;
    mesh->mNumVertices = static_cast<unsigned>(spec.positions.size());

    if (!spec.positions.empty()) {
        mesh->mVertices = new aiVector3D[spec.positions.size()];
        std::memcpy(mesh->mVertices, spec.positions.data(),
                spec.positions.size() * sizeof(aiVector3D));
    }
    if (!spec.normals.empty()) {
        mesh->mNormals = new aiVector3D[spec.normals.size()];
        std::memcpy(mesh->mNormals, spec.normals.data(),
                spec.normals.size() * sizeof(aiVector3D));
    }
    if (!spec.texCoords.empty()) {
        mesh->mTextureCoords[0] = new aiVector3D[spec.texCoords.size()];
        std::memcpy(mesh->mTextureCoords[0], spec.texCoords.data(),
                spec.texCoords.size() * sizeof(aiVector3D));
        mesh->mNumUVComponents[0] = 2;
    }
    if (!spec.colors.empty()) {
        mesh->mColors[0] = new aiColor4D[spec.colors.size()];
        std::memcpy(mesh->mColors[0], spec.colors.data(),
                spec.colors.size() * sizeof(aiColor4D));
    }

    if (!spec.faces.empty()) {
        mesh->mNumFaces = static_cast<unsigned>(spec.faces.size());
        mesh->mFaces = new aiFace[spec.faces.size()];
        for (unsigned f = 0; f < spec.faces.size(); ++f) {
            const std::vector<unsigned> &face = spec.faces[f];
            mesh->mFaces[f].mNumIndices = static_cast<unsigned>(face.size());
            mesh->mFaces[f].mIndices = new unsigned[face.size()];
            std::memcpy(mesh->mFaces[f].mIndices, face.data(), face.size() * sizeof(unsigned));
        }
    }

    if (!spec.bones.empty()) {
        mesh->mNumBones = static_cast<unsigned>(spec.bones.size());
        mesh->mBones = new aiBone *[spec.bones.size()];
        for (unsigned b = 0; b < spec.bones.size(); ++b) {
            aiBone *bone = new aiBone();
            bone->mName.Set(spec.bones[b].name.c_str());
            bone->mOffsetMatrix = spec.bones[b].offset;
            bone->mNumWeights = static_cast<unsigned>(spec.bones[b].weights.size());
            if (!spec.bones[b].weights.empty()) {
                bone->mWeights = new aiVertexWeight[spec.bones[b].weights.size()];
                for (unsigned w = 0; w < spec.bones[b].weights.size(); ++w) {
                    bone->mWeights[w].mVertexId = spec.bones[b].weights[w].vertex;
                    bone->mWeights[w].mWeight = spec.bones[b].weights[w].weight;
                }
            }
            mesh->mBones[b] = bone;
        }
    }

    if (!spec.anims.empty()) {
        mesh->mNumAnimMeshes = static_cast<unsigned>(spec.anims.size());
        mesh->mAnimMeshes = new aiAnimMesh *[spec.anims.size()];
        for (unsigned a = 0; a < spec.anims.size(); ++a) {
            const MeshSpec::AnimSpec &animSpec = spec.anims[a];
            aiAnimMesh *anim = new aiAnimMesh();
            anim->mName.Set(animSpec.name.c_str());
            anim->mNumVertices = mesh->mNumVertices;
            if (!animSpec.positions.empty()) {
                anim->mVertices = new aiVector3D[animSpec.positions.size()];
                std::memcpy(anim->mVertices, animSpec.positions.data(),
                        animSpec.positions.size() * sizeof(aiVector3D));
            }
            if (!animSpec.normals.empty()) {
                anim->mNormals = new aiVector3D[animSpec.normals.size()];
                std::memcpy(anim->mNormals, animSpec.normals.data(),
                        animSpec.normals.size() * sizeof(aiVector3D));
            }
            if (!animSpec.tangents.empty()) {
                anim->mTangents = new aiVector3D[animSpec.tangents.size()];
                std::memcpy(anim->mTangents, animSpec.tangents.data(),
                        animSpec.tangents.size() * sizeof(aiVector3D));
            }
            if (!animSpec.bitangents.empty()) {
                anim->mBitangents = new aiVector3D[animSpec.bitangents.size()];
                std::memcpy(anim->mBitangents, animSpec.bitangents.data(),
                        animSpec.bitangents.size() * sizeof(aiVector3D));
            }
            if (!animSpec.texCoords.empty()) {
                anim->mTextureCoords[0] = new aiVector3D[animSpec.texCoords.size()];
                std::memcpy(anim->mTextureCoords[0], animSpec.texCoords.data(),
                        animSpec.texCoords.size() * sizeof(aiVector3D));
            }
            if (!animSpec.colors.empty()) {
                anim->mColors[0] = new aiColor4D[animSpec.colors.size()];
                std::memcpy(anim->mColors[0], animSpec.colors.data(),
                        animSpec.colors.size() * sizeof(aiColor4D));
            }
            mesh->mAnimMeshes[a] = anim;
        }
    }

    return mesh;
}

/// Linear blend skinning of one vertex. `angles` supplies one rotation angle
/// per bone slot; bone offset matrices come from the mesh.
aiVector3D SkinnedPosition(const aiMesh *mesh, unsigned vertex,
        const std::vector<ai_real> &angles) {
    aiVector3D result(0.f, 0.f, 0.f);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        if (b >= angles.size()) {
            continue;
        }
        aiMatrix4x4 rotation;
        aiMatrix4x4::RotationX(angles[b], rotation);
        const aiVector3D boneSpace = bone->mOffsetMatrix * mesh->mVertices[vertex];
        const aiVector3D skinned = rotation * boneSpace;
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            if (bone->mWeights[w].mVertexId == vertex) {
                result += skinned * bone->mWeights[w].mWeight;
            }
        }
    }
    return result;
}

/// Collects the skinned position of every face corner before welding.
struct CornerSkinnings {
    struct Corner {
        unsigned face;
        unsigned corner;
        aiVector3D position[2];
    };
    std::vector<Corner> corners;
};

CornerSkinnings CaptureCorners(const aiMesh *mesh) {
    CornerSkinnings captured;
    const std::vector<ai_real> anglesA = { 0.0f, 0.6f };
    const std::vector<ai_real> anglesB = { 1.1f, -0.4f };
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        for (unsigned c = 0; c < mesh->mFaces[f].mNumIndices; ++c) {
            CornerSkinnings::Corner corner;
            corner.face = f;
            corner.corner = c;
            corner.position[0] = SkinnedPosition(mesh, mesh->mFaces[f].mIndices[c], anglesA);
            corner.position[1] = SkinnedPosition(mesh, mesh->mFaces[f].mIndices[c], anglesB);
            captured.corners.push_back(corner);
        }
    }
    return captured;
}

/// Verifies each face corner still skins to the captured position after welding.
void CheckCornersPreserved(const aiMesh *mesh, const CornerSkinnings &before,
        const std::vector<ai_real> &anglesA, const std::vector<ai_real> &anglesB,
        double positionTolerance) {
    for (const auto &corner : before.corners) {
        const unsigned newIndex = mesh->mFaces[corner.face].mIndices[corner.corner];
        WELD_CHECK(newIndex < mesh->mNumVertices);
        const aiVector3D afterA = SkinnedPosition(mesh, newIndex, anglesA);
        const aiVector3D afterB = SkinnedPosition(mesh, newIndex, anglesB);
        WELD_CHECK((afterA - corner.position[0]).Length() <=
                   static_cast<ai_real>(positionTolerance));
        WELD_CHECK((afterB - corner.position[1]).Length() <=
                   static_cast<ai_real>(positionTolerance));
    }
}

/// Post-conditions every processed bone record has to satisfy.
void CheckBoneInvariants(const aiMesh *mesh) {
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        if (bone->mNumWeights == 0) {
            WELD_CHECK(bone->mWeights == nullptr);
            continue;
        }
        WELD_CHECK(bone->mWeights != nullptr);
        std::vector<unsigned> ids;
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const unsigned id = bone->mWeights[w].mVertexId;
            WELD_CHECK(id < mesh->mNumVertices);
            for (unsigned other : ids) {
                WELD_CHECK(other != id); // no duplicate new vertex ids per bone
            }
            ids.push_back(id);
        }
    }
}

/// Counts how often each new vertex index is referenced by faces.
unsigned CountReferencedVertices(const aiMesh *mesh) {
    std::vector<bool> referenced(mesh->mNumVertices, false);
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        for (unsigned c = 0; c < mesh->mFaces[f].mNumIndices; ++c) {
            referenced[mesh->mFaces[f].mIndices[c]] = true;
        }
    }
    unsigned count = 0;
    for (bool used : referenced) {
        count += used ? 1 : 0;
    }
    return count;
}

// ------------------------------------------------------------------------------------------------
// epsilon_candidates
// ------------------------------------------------------------------------------------------------
WELD_CASE(epsilon_candidates) {
    Assimp::JoinVerticesProcess process;

    // A triangle with the second corner shifted by `dx`.
    auto buildTriangle = [](float dx) {
        MeshSpec spec;
        spec.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(dx, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        return BuildMesh(spec);
    };

    // Exact copies still weld.
    {
        std::unique_ptr<aiMesh> mesh(buildTriangle(0.f));
        const unsigned oldCount = mesh->mNumVertices;
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
        WELD_CHECK(mesh->mNumVertices < oldCount);
        WELD_CHECK(CountReferencedVertices(mesh.get()) == 2);
    }

    // 5e-6 is inside the 1e-5 distance tolerance: it must weld.
    {
        std::unique_ptr<aiMesh> mesh(buildTriangle(5e-6f));
        const unsigned oldCount = mesh->mNumVertices;
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
        WELD_CHECK(mesh->mNumVertices < oldCount);
        // The representative keeps its original position untouched.
        WELD_CHECK(mesh->mVertices[0].x == 0.f);
        WELD_CHECK(mesh->mFaces[0].mIndices[1] == 0);
        WELD_CHECK(mesh->mFaces[0].mIndices[2] == 1);
    }

    // 2e-5 is outside: it must stay split.
    {
        std::unique_ptr<aiMesh> mesh(buildTriangle(2e-5f));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 3);
    }

    // Crossing a bucket border: the spatial cell size is 4e-5, so 3.9e-5 and
    // 4.1e-5 land in different cells (floor(x/cell) gives 0 and 1) although
    // they are only 2e-6 apart. Both positive and negative coordinates have to
    // be found in the neighbouring cell.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(3.9e-5f, 0.f, 0.f), aiVector3D(4.1e-5f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
        WELD_CHECK(CountReferencedVertices(mesh.get()) == 2);
    }
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(-3.9e-5f, 0.f, 0.f), aiVector3D(-4.1e-5f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
    }

    // The same border crossing in all three axes at once, and in a far-away
    // (but small relative to the magnitude) region where float spacing is
    // coarser than the tolerance: the bucket search must still find it.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(3.9e-5f, 3.9e-5f, 3.9e-5f),
            aiVector3D(4.1e-5f, 4.1e-5f, 4.1e-5f), aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
    }
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(1.0000096e8f, 0.f, 0.f),
            aiVector3D(1.0000097e8f, 0.f, 0.f), aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        // The two large positions are 1 unit apart (float spacing here is
        // 8 units), well above the tolerance: they must not weld.
        WELD_CHECK(mesh->mNumVertices == 3);
    }

    // Approximate normals / UVs / colours use the same distance tolerance.
    {
        MeshSpec mergeSpec;
        mergeSpec.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(5e-6f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        mergeSpec.normals = { aiVector3D(0.f, 0.f, 1.f), aiVector3D(3e-6f, 0.f, 1.f),
            aiVector3D(0.f, 0.f, 1.f) };
        mergeSpec.texCoords = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(4e-6f, 2e-6f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        mergeSpec.colors = { aiColor4D(1.f, 0.f, 0.f, 1.f), aiColor4D(1.f, 4e-6f, 0.f, 1.f),
            aiColor4D(1.f, 0.f, 0.f, 1.f) };
        mergeSpec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> merged(BuildMesh(mergeSpec));
        process.ProcessMesh(merged.get(), 0);
        WELD_CHECK(merged->mNumVertices == 2);

        // ... but a normal that is 2e-5 off must not weld even though the
        // positions match.
        MeshSpec splitSpec = mergeSpec;
        splitSpec.normals[1] = aiVector3D(2e-5f, 0.f, 1.f);
        std::unique_ptr<aiMesh> split(BuildMesh(splitSpec));
        process.ProcessMesh(split.get(), 0);
        WELD_CHECK(split->mNumVertices == 3);

        // UV and colour differences above the tolerance also block welding.
        splitSpec = mergeSpec;
        splitSpec.texCoords[1] = aiVector3D(2e-5f, 0.f, 0.f);
        std::unique_ptr<aiMesh> splitUV(BuildMesh(splitSpec));
        process.ProcessMesh(splitUV.get(), 0);
        WELD_CHECK(splitUV->mNumVertices == 3);

        splitSpec = mergeSpec;
        splitSpec.colors[1] = aiColor4D(1.f, 2e-5f, 0.f, 1.f);
        std::unique_ptr<aiMesh> splitColor(BuildMesh(splitSpec));
        process.ProcessMesh(splitColor.get(), 0);
        WELD_CHECK(splitColor->mNumVertices == 3);
    }
}

// ------------------------------------------------------------------------------------------------
// representative_chain
// ------------------------------------------------------------------------------------------------
WELD_CASE(representative_chain) {
    Assimp::JoinVerticesProcess process;

    // A=(0), B=(8e-6), C=(1.6e-5). A~B and B~C, but A!~C. B joins A; because
    // the relation is not transitive, C must not follow B into A. C therefore
    // becomes its own representative.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(8e-6f, 0.f, 0.f),
            aiVector3D(1.6e-5f, 0.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 2);
        // The representative is the earliest original vertex and keeps its
        // position unchanged (no averaging, no movement).
        WELD_CHECK(mesh->mVertices[0].x == 0.f);
        WELD_CHECK(std::fabs(mesh->mVertices[1].x - 1.6e-5f) < 1e-9f);
        // The face corner order stays the same: corner 0 -> 0, 1 -> 0, 2 -> 1.
        WELD_CHECK(mesh->mFaces[0].mIndices[0] == 0);
        WELD_CHECK(mesh->mFaces[0].mIndices[1] == 0);
        WELD_CHECK(mesh->mFaces[0].mIndices[2] == 1);
    }

    // Same chain, but the earliest point is not a valid representative for the
    // last one even though it is close to the middle point. Also check that the
    // scan is by original order, not by bucket order: put the chain on the
    // negative side and across several bucket boundaries.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(-3e-5f, 0.f, 0.f), aiVector3D(-2.2e-5f, 0.f, 0.f),
            aiVector3D(-1.4e-5f, 0.f, 0.f), aiVector3D(-0.6e-5f, 0.f, 0.f) };
        spec.faces = { { 0, 1, 2, 3 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        const unsigned oldCount = mesh->mNumVertices;
        process.ProcessMesh(mesh.get(), 0);
        // -3e-5 joins nothing; -2.2e-5 joins -3e-5? distance 8e-6 => yes.
        // -1.4e-5: distance to -3e-5 is 1.6e-5 (no), to -2.2e-5 is 8e-6 but
        // -2.2e-5 was joined (not a representative) => it becomes a
        // representative itself. -0.6e-5 joins -1.4e-5 (8e-6).
        WELD_CHECK(mesh->mNumVertices == 2);
        WELD_CHECK(mesh->mNumVertices < oldCount);
        WELD_CHECK(mesh->mVertices[0].x == -3e-5f);
        WELD_CHECK(std::fabs(mesh->mVertices[1].x - (-1.4e-5f)) < 1e-9f);
        WELD_CHECK(mesh->mFaces[0].mIndices[0] == 0);
        WELD_CHECK(mesh->mFaces[0].mIndices[1] == 0);
        WELD_CHECK(mesh->mFaces[0].mIndices[2] == 1);
        WELD_CHECK(mesh->mFaces[0].mIndices[3] == 1);
    }

    // Very large but finite coordinates must not overflow the cell quantization
    // and must never be merged with unrelated vertices.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(1e20f, 0.f, 0.f), aiVector3D(-1e20f, 0.f, 0.f),
            aiVector3D(3e38f, 0.f, 0.f), aiVector3D(-3e38f, 0.f, 0.f) };
        spec.faces = { { 0, 1, 2 }, { 0, 0, 3 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        // Nothing comes close to anything else: all four used vertices survive
        // and the clamping of huge coordinates cannot merge unrelated points.
        WELD_CHECK(mesh->mNumVertices == 4);
        WELD_CHECK(CountReferencedVertices(mesh.get()) == 4);
        WELD_CHECK(mesh->mVertices[0].x == 1e20f);
        WELD_CHECK(mesh->mVertices[1].x == -1e20f);
        WELD_CHECK(mesh->mVertices[2].x == 3e38f);
        WELD_CHECK(mesh->mVertices[3].x == -3e38f);
        WELD_CHECK(mesh->mFaces[1].mIndices[0] == 0);
        WELD_CHECK(mesh->mFaces[1].mIndices[2] == 3);
    }

    // Non-finite positions are never welded, even onto an identical-looking
    // vertex: NaN comparisons cannot establish equality.
    {
        MeshSpec spec;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        spec.positions = { aiVector3D(nan, 0.f, 0.f), aiVector3D(nan, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        spec.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 3);
        WELD_CHECK(mesh->mFaces[0].mIndices[0] != mesh->mFaces[0].mIndices[1]);
    }

    // Unreferenced vertices are dropped and cannot become representatives.
    {
        MeshSpec spec;
        spec.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
            aiVector3D(5.f, 5.f, 5.f) };
        spec.faces = { { 0, 1, 1 } };
        std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
        const unsigned oldCount = mesh->mNumVertices;
        process.ProcessMesh(mesh.get(), 0);
        WELD_CHECK(mesh->mNumVertices == 1);
        WELD_CHECK(mesh->mNumVertices < oldCount);
        WELD_CHECK(mesh->mVertices[0].x == 0.f);
        for (unsigned c = 0; c < mesh->mFaces[0].mNumIndices; ++c) {
            WELD_CHECK(mesh->mFaces[0].mIndices[c] == 0);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// skin_signatures
// ------------------------------------------------------------------------------------------------
WELD_CASE(skin_signatures) {
    Assimp::JoinVerticesProcess process;

    const ai_real angleA0 = 0.0f;
    const ai_real angleA1 = 0.6f;
    const ai_real angleB0 = 1.1f;
    const ai_real angleB1 = -0.4f;
    const std::vector<ai_real> anglesA = { angleA0, angleA1 };
    const std::vector<ai_real> anglesB = { angleB0, angleB1 };

    // Four coincident vertices (1..4) at the same position, plus two anchors.
    // Each coincident vertex has a different bone signature. Exact copies
    // (including an explicit zero weight on the other slot) must weld, different
    // signatures must not.
    MeshSpec spec;
    spec.positions = {
        aiVector3D(0.f, 0.f, 0.f),
        aiVector3D(1.f, 0.f, 0.f), // 1: left only
        aiVector3D(1.f, 0.f, 0.f), // 2: right only
        aiVector3D(1.f, 0.f, 0.f), // 3: left only + explicit zero right
        aiVector3D(1.f, 0.f, 0.f), // 4: half/half
        aiVector3D(0.f, 1.f, 0.f),
    };
    spec.faces = { { 0, 1, 2 }, { 0, 3, 4 }, { 0, 1, 5 } };
    // Left and right share a name prefix on purpose; identities are bone slots.
    spec.bones = {
        { "left", MakeOffset(0.2f, 0.f, 0.f, 0.f), { { 1, 1.f }, { 3, 1.f }, { 4, 0.5f } } },
        { "right", MakeOffset(-0.35f, 1.f, 0.f, 0.f), { { 2, 1.f }, { 3, 0.f }, { 4, 0.5f } } },
    };

    std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
    const unsigned oldCount = mesh->mNumVertices;
    const CornerSkinnings before = CaptureCorners(mesh.get());
    process.ProcessMesh(mesh.get(), 0);

    // v1, v2, v4 and both anchors survive; the zero-weight duplicate v3 joins
    // v1 (zero weight == missing weight). Old count 6 -> new count 5.
    WELD_CHECK(mesh->mNumVertices == 5);
    WELD_CHECK(mesh->mNumVertices < oldCount);
    CheckBoneInvariants(mesh.get());
    CheckCornersPreserved(mesh.get(), before, anglesA, anglesB, 1e-6);

    // The merged vertex keeps the representative's original attributes: the
    // left-only vertex 1 is the earliest candidate for vertex 3.
    const unsigned face1corner1 = mesh->mFaces[0].mIndices[1];
    const unsigned face1corner2 = mesh->mFaces[0].mIndices[2];
    const unsigned face2corner1 = mesh->mFaces[1].mIndices[1];
    const unsigned face2corner2 = mesh->mFaces[1].mIndices[2];
    WELD_CHECK(face1corner1 == face2corner1);   // v1 and v3 share a new index
    WELD_CHECK(face1corner2 != face2corner2);   // v2 (right) stays separate
    WELD_CHECK(mesh->mFaces[2].mIndices[2] != face1corner1); // half/half stays
    // Skinning under two different pose sets:
    // vertex 1 (left only) moves with left, vertex 2 with right.
    const aiVector3D left1 = SkinnedPosition(mesh.get(), face1corner1, anglesA);
    const aiVector3D right2 = SkinnedPosition(mesh.get(), face1corner2, anglesA);
    // left bone: identity rotation in its offset, so x stays 1; right bone:
    // offset translates x by +1, so the all-right vertex skins to x == 2.
    WELD_CHECK(std::fabs(left1.x - 1.f) < 1e-6f);
    WELD_CHECK(std::fabs(right2.x - 2.f) < 1e-6f);
    WELD_CHECK(std::fabs(left1.x - right2.x) > 1e-5f);
    // Bone slots are independent even if names would collide: compare an
    // all-left vertex and an all-right vertex at the same position, both with
    // weight 1. Their skinning differs, so they must not have been merged.
    {
        MeshSpec sameName;
        sameName.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(1.f, 0.f, 0.f),
            aiVector3D(1.f, 0.f, 0.f), aiVector3D(0.f, 1.f, 0.f) };
        sameName.faces = { { 0, 1, 3 }, { 0, 2, 3 } };
        sameName.bones = {
            { "arm", MakeOffset(0.f, 0.f, 0.f, 0.f), { { 1, 1.f } } },
            { "arm", MakeOffset(0.9f, 2.f, 0.f, 0.f), { { 2, 1.f } } },
        };
        std::unique_ptr<aiMesh> sameNameMesh(BuildMesh(sameName));
        const unsigned oldCount = sameNameMesh->mNumVertices;
        process.ProcessMesh(sameNameMesh.get(), 0);
        WELD_CHECK(sameNameMesh->mNumVertices == oldCount);
        WELD_CHECK(sameNameMesh->mFaces[0].mIndices[1] != sameNameMesh->mFaces[1].mIndices[1]);
    }

    // Weight sums are not normalized: 0.5+0.5 and 0.7+0.3 are different.
    {
        MeshSpec sums;
        sums.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(1e-6f, 0.f, 0.f),
            aiVector3D(1e-6f, 0.f, 0.f), aiVector3D(0.f, 1.f, 0.f) };
        sums.faces = { { 0, 1, 3 }, { 0, 2, 3 } };
        sums.bones = {
            { "b0", MakeOffset(0.f, 0.f, 0.f, 0.f), { { 1, 0.5f }, { 2, 0.7f } } },
            { "b1", MakeOffset(0.f, 0.f, 0.f, 0.f), { { 1, 0.5f }, { 2, 0.3f } } },
        };
        std::unique_ptr<aiMesh> sumsMesh(BuildMesh(sums));
        const unsigned oldCount = sumsMesh->mNumVertices;
        process.ProcessMesh(sumsMesh.get(), 0);
        WELD_CHECK(sumsMesh->mNumVertices == oldCount);
        WELD_CHECK(sumsMesh->mFaces[0].mIndices[1] != sumsMesh->mFaces[1].mIndices[1]);
    }
}

// ------------------------------------------------------------------------------------------------
// animated_attributes
// ------------------------------------------------------------------------------------------------
WELD_CASE(animated_attributes) {
    Assimp::JoinVerticesProcess process;

    // Four coincident vertices (1..3) with identical base data. The animation
    // targets distinguish them: target 0 stores positions in which vertex 3
    // differs, target 1 stores normals in which vertex 3 differs again. Only
    // vertices 1 and 2 have identical stored streams and may merge.
    MeshSpec spec;
    spec.positions = { aiVector3D(100.f, 0.f, 0.f), aiVector3D(1e-6f, 0.f, 0.f),
        aiVector3D(1e-6f, 0.f, 0.f), aiVector3D(1e-6f, 0.f, 0.f),
        aiVector3D(0.f, 100.f, 0.f) };
    spec.normals = { aiVector3D(0.f, 0.f, 1.f), aiVector3D(0.f, 0.f, 1.f),
        aiVector3D(0.f, 0.f, 1.f), aiVector3D(0.f, 0.f, 1.f), aiVector3D(0.f, 0.f, 1.f) };
    spec.faces = { { 0, 1, 4 }, { 0, 2, 4 }, { 0, 3, 4 } };
    MeshSpec::AnimSpec target0;
    target0.name = "target0";
    target0.positions = { aiVector3D(1.f, 0.f, 0.f), aiVector3D(1.f, 0.f, 0.f),
        aiVector3D(1.f, 0.f, 0.f), aiVector3D(2.f, 0.f, 0.f), aiVector3D(1.f, 1.f, 0.f) };
    MeshSpec::AnimSpec target1;
    target1.name = "target1";
    target1.positions = { aiVector3D(0.5f, 0.f, 0.f), aiVector3D(0.5f, 0.f, 0.f),
        aiVector3D(0.5f, 0.f, 0.f), aiVector3D(0.5f, 0.f, 0.f), aiVector3D(0.5f, 1.f, 0.f) };
    target1.normals = { aiVector3D(0.f, 1.f, 0.f), aiVector3D(0.f, 1.f, 0.f),
        aiVector3D(0.f, 1.f, 0.f), aiVector3D(1.f, 0.f, 0.f), aiVector3D(0.f, 1.f, 0.f) };
    // target2 has no arrays at all: it inherits every base attribute.
    MeshSpec::AnimSpec target2;
    target2.name = "target2";
    spec.anims = { target0, target1, target2 };

    std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
    const unsigned oldCount = mesh->mNumVertices;
    process.ProcessMesh(mesh.get(), 0);

    // 1 and 2 merge (their stored target streams are identical); vertex 3 is
    // separated by both target 0 and target 1: 5 -> 4.
    WELD_CHECK(mesh->mNumVertices == 4);
    WELD_CHECK(mesh->mNumVertices < oldCount);
    WELD_CHECK(mesh->mFaces[0].mIndices[1] == mesh->mFaces[1].mIndices[1]);
    WELD_CHECK(mesh->mFaces[1].mIndices[1] != mesh->mFaces[2].mIndices[1]);
    WELD_CHECK(mesh->mFaces[0].mIndices[1] != mesh->mFaces[2].mIndices[1]);

    // All targets were remapped with the same mapping and keep their data.
    WELD_CHECK(mesh->mNumAnimMeshes == 3);
    for (unsigned a = 0; a < 2; ++a) {
        const aiAnimMesh *anim = mesh->mAnimMeshes[a];
        WELD_CHECK(anim->mNumVertices == mesh->mNumVertices);
        WELD_CHECK(anim->mVertices != nullptr);
    }
    // An absent stream stays absent and inherits the base attribute.
    WELD_CHECK(mesh->mAnimMeshes[2]->mNumVertices == mesh->mNumVertices);
    WELD_CHECK(mesh->mAnimMeshes[2]->mVertices == nullptr);
    WELD_CHECK(mesh->mAnimMeshes[2]->mNormals == nullptr);
    // target0 positions of the merged corner pair are equal.
    const aiAnimMesh *anim0 = mesh->mAnimMeshes[0];
    const unsigned mergedIndex = mesh->mFaces[0].mIndices[1];
    WELD_CHECK(anim0->mVertices[mergedIndex].x == 1.f);
    // target1 normals of the merged pair agree, while the separate
    // representative keeps its own stored normal.
    const aiAnimMesh *anim1 = mesh->mAnimMeshes[1];
    const unsigned rep3 = mesh->mFaces[2].mIndices[1];
    WELD_CHECK(anim1->mNormals[mergedIndex] == anim1->mNormals[mesh->mFaces[1].mIndices[1]]);
    WELD_CHECK(anim1->mNormals[mergedIndex] != anim1->mNormals[rep3]);
    WELD_CHECK(anim1->mNormals[mergedIndex].y == 1.f);
    WELD_CHECK(anim1->mNormals[rep3].x == 1.f);

    // A mesh whose only animation target has no arrays at all must still weld:
    // absent arrays inherit the base data, they do not disable welding.
    {
        MeshSpec inherits;
        inherits.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        inherits.faces = { { 0, 1, 2 } };
        inherits.anims = { target2 };
        std::unique_ptr<aiMesh> inheritsMesh(BuildMesh(inherits));
        process.ProcessMesh(inheritsMesh.get(), 0);
        WELD_CHECK(inheritsMesh->mNumVertices == 2);
        WELD_CHECK(inheritsMesh->mAnimMeshes[0]->mNumVertices == 2);
        WELD_CHECK(inheritsMesh->mAnimMeshes[0]->mVertices == nullptr);
    }

    // An animation target that exists but only stores some streams: the
    // present stream blocks a merge, the absent one does not.
    {
        MeshSpec partial;
        partial.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        partial.texCoords = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(1e-6f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        partial.faces = { { 0, 1, 2 } };
        MeshSpec::AnimSpec uvOnly;
        uvOnly.name = "uvOnly";
        uvOnly.texCoords = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.5f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        partial.anims = { uvOnly };
        std::unique_ptr<aiMesh> partialMesh(BuildMesh(partial));
        const unsigned oldCount = partialMesh->mNumVertices;
        process.ProcessMesh(partialMesh.get(), 0);
        // The target's stored UV array differs for vertex 1, so nothing merges.
        WELD_CHECK(partialMesh->mNumVertices == oldCount);
        const aiAnimMesh *anim = partialMesh->mAnimMeshes[0];
        WELD_CHECK(anim->mNumVertices == partialMesh->mNumVertices);
        WELD_CHECK(anim->mTextureCoords[0][0].x == 0.f);
        WELD_CHECK(anim->mTextureCoords[0][1].x == 0.5f);
        WELD_CHECK(anim->mTextureCoords[0][2].x == 0.f);
    }
}

// ------------------------------------------------------------------------------------------------
// remap_and_legacy
// ------------------------------------------------------------------------------------------------
WELD_CASE(remap_and_legacy) {
    Assimp::JoinVerticesProcess process;

    // Mesh with:
    //  - v0/v1 coincident with the same weight on bone "left" => collapse onto
    //    the earliest representative, and the duplicate weight entry must be
    //    dropped instead of duplicated in the bone record;
    //  - v2 unreferenced but influenced by bone "right" => must disappear
    //    together with its weight;
    //  - bone "unused" only influences the unreferenced vertex => empty record.
    MeshSpec spec;
    spec.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
        aiVector3D(9.f, 9.f, 9.f), aiVector3D(0.f, 1.f, 0.f) };
    spec.faces = { { 0, 1, 3 } };
    spec.materialIndex = 2;
    spec.bones = {
        { "left", MakeOffset(0.1f, 0.f, 0.f, 0.f), { { 0, 0.75f }, { 1, 0.75f } } },
        { "right", MakeOffset(0.3f, 1.f, 2.f, 3.f), { { 2, 1.f } } },
        { "unused", MakeOffset(0.5f, -1.f, 0.f, 0.f), { { 2, 0.5f } } },
    };

    std::unique_ptr<aiMesh> mesh(BuildMesh(spec));
    const std::string leftName = mesh->mBones[0]->mName.data;
    const aiMatrix4x4 leftOffset = mesh->mBones[0]->mOffsetMatrix;
    const aiMatrix4x4 rightOffset = mesh->mBones[1]->mOffsetMatrix;
    const aiMatrix4x4 unusedOffset = mesh->mBones[2]->mOffsetMatrix;
    process.ProcessMesh(mesh.get(), 0);

    WELD_CHECK(mesh->mNumVertices == 2); // v0, v3 (v1 merged, v2 dropped)
    WELD_CHECK(mesh->mMaterialIndex == 2);
    WELD_CHECK(mesh->mPrimitiveTypes == aiPrimitiveType_TRIANGLE);
    WELD_CHECK(mesh->mNumBones == 3); // bone objects and their order survive
    CheckBoneInvariants(mesh.get());

    WELD_CHECK(leftName == mesh->mBones[0]->mName.data);
    WELD_CHECK(mesh->mBones[0]->mNumWeights == 1);
    WELD_CHECK(mesh->mBones[0]->mWeights[0].mVertexId == mesh->mFaces[0].mIndices[0]);
    WELD_CHECK(mesh->mBones[0]->mWeights[0].mWeight == 0.75f); // representative's weight
    WELD_CHECK(mesh->mBones[0]->mOffsetMatrix == leftOffset);
    WELD_CHECK(std::string(mesh->mBones[1]->mName.data) == "right");
    WELD_CHECK(mesh->mBones[1]->mNumWeights == 0);
    WELD_CHECK(mesh->mBones[1]->mWeights == nullptr);
    WELD_CHECK(mesh->mBones[1]->mOffsetMatrix == rightOffset);
    WELD_CHECK(std::string(mesh->mBones[2]->mName.data) == "unused");
    WELD_CHECK(mesh->mBones[2]->mNumWeights == 0);
    WELD_CHECK(mesh->mBones[2]->mWeights == nullptr);
    WELD_CHECK(mesh->mBones[2]->mOffsetMatrix == unusedOffset);
    // Old vertex ids must not survive: the only recorded id is a valid new
    // index and the removed vertex 2 is gone.
    WELD_CHECK(mesh->mBones[0]->mWeights[0].mVertexId < mesh->mNumVertices);

    // Execute() on a scene with two meshes. The second mesh has different data
    // and must be processed independently (no cached mapping from mesh 0).
    {
        aiScene *scene = new aiScene();
        scene->mNumMeshes = 2;
        scene->mMeshes = new aiMesh *[2];

        MeshSpec first;
        first.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f), aiVector3D(7.f, 7.f, 7.f) };
        first.faces = { { 0, 1, 2 } };
        // Vertex 3 is unreferenced but still carries a weight; it must vanish
        // together with its weight instead of leaving a stale old vertex id.
        first.bones = {
            { "first", MakeOffset(0.f, 0.f, 0.f, 0.f),
                { { 0, 1.f }, { 1, 1.f }, { 3, 0.5f } } },
        };
        scene->mMeshes[0] = BuildMesh(first);

        MeshSpec second;
        second.positions = { aiVector3D(5.f, 0.f, 0.f), aiVector3D(5.f, 0.f, 0.f),
            aiVector3D(5.f, 1.f, 0.f), aiVector3D(6.f, 0.f, 0.f) };
        second.faces = { { 0, 1, 2 }, { 0, 0, 3 } };
        second.materialIndex = 1;
        scene->mMeshes[1] = BuildMesh(second);

        scene->mNumMaterials = 0;
        scene->mMaterials = nullptr;
        scene->mFlags = 0;

        process.Execute(scene);
        WELD_CHECK(scene->mNumMeshes == 2);
        WELD_CHECK(scene->mMeshes[0]->mNumVertices == 2);
        WELD_CHECK(scene->mMeshes[1]->mNumVertices == 3);
        WELD_CHECK(scene->mMeshes[1]->mMaterialIndex == 1);
        WELD_CHECK(scene->mMeshes[1]->mPrimitiveTypes == aiPrimitiveType_TRIANGLE);
        WELD_CHECK((scene->mFlags & AI_SCENE_FLAGS_NON_VERBOSE_FORMAT) != 0);
        for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
            WELD_CHECK(scene->mMeshes[m]->mNumVertices <= 3);
            WELD_CHECK(CountReferencedVertices(scene->mMeshes[m]) ==
                       scene->mMeshes[m]->mNumVertices);
            CheckBoneInvariants(scene->mMeshes[m]);
        }
        // Mesh 0: the unreferenced vertex 3 is gone; vertices 0 and 1 welded
        // onto the same new id, so only one weight entry remains and no old
        // vertex id survives.
        WELD_CHECK(scene->mMeshes[0]->mNumBones == 1);
        WELD_CHECK(scene->mMeshes[0]->mBones[0]->mNumWeights == 1);
        WELD_CHECK(scene->mMeshes[0]->mBones[0]->mWeights[0].mVertexId ==
                   scene->mMeshes[0]->mFaces[0].mIndices[0]);
        WELD_CHECK(scene->mMeshes[0]->mBones[0]->mWeights[0].mWeight == 1.f);

        delete scene;
    }

    // A plain (unskinned, unanimated) mesh still welds exactly.
    {
        MeshSpec plain;
        plain.positions = { aiVector3D(0.f, 0.f, 0.f), aiVector3D(0.f, 0.f, 0.f),
            aiVector3D(0.f, 1.f, 0.f) };
        plain.faces = { { 0, 1, 2 } };
        std::unique_ptr<aiMesh> plainMesh(BuildMesh(plain));
        process.ProcessMesh(plainMesh.get(), 0);
        WELD_CHECK(plainMesh->mNumVertices == 2);
        WELD_CHECK(plainMesh->mNumFaces == 1);
        WELD_CHECK(plainMesh->mFaces[0].mNumIndices == 3);
        WELD_CHECK(plainMesh->mBones == nullptr);
    }
}

} // namespace weld
