/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

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
---------------------------------------------------------------------------
*/

/// @file  morph_bake.cpp
/// @brief Task tests for baking node transformations into morph targets
///   (AI_CONFIG_PP_PTV_PRESERVE_MORPHS) and for morph-aware mesh merging
///   (AI_INT_MERGE_MESHES_PRESERVE_MORPHS).

#include "task_support/check.h"

#include "PostProcessing/PretransformVertices.h"

#include <assimp/SceneCombiner.h>
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

constexpr float kEps = 1e-5f;
constexpr unsigned kNumVerts = 4;

bool Near(float a, float b) {
    return std::fabs(a - b) <= kEps * (1.0f + std::fabs(b));
}

bool NearV(const aiVector3D &a, const aiVector3D &b) {
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

bool NearC(const aiColor4D &a, const aiColor4D &b) {
    return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a);
}

// Reference transform rules, evaluated independently from the step.
aiVector3D RefPosition(const aiMatrix4x4 &m, const aiVector3D &v) {
    return m * v;
}

aiVector3D RefDirection(const aiMatrix4x4 &m, const aiVector3D &v) {
    return (aiMatrix3x3(m).Inverse().Transpose() * v).Normalize();
}

// Which replacement streams a morph target carries.
struct MorphSpec {
    const char *name;
    float weight;
    bool positions;
    bool normals;
    bool tangents;
    bool uv2;
    bool color1;
};

// Base geometry shared by the test meshes: two triangles, four vertices.
const aiVector3D kBaseVerts[kNumVerts] = {
        aiVector3D(0.0f, 0.0f, 0.0f),
        aiVector3D(1.0f, 0.0f, 0.0f),
        aiVector3D(1.0f, 1.0f, 0.0f),
        aiVector3D(0.0f, 2.0f, 0.0f),
};

const aiVector3D kBaseNormals[kNumVerts] = {
        aiVector3D(0.0f, 0.0f, 1.0f),
        aiVector3D(0.0f, 1.0f, 0.0f),
        aiVector3D(1.0f, 0.0f, 0.0f),
        aiVector3D(0.0f, -1.0f, 0.0f),
};

aiVector3D TargetPosition(unsigned i) {
    // Deliberately far away from the base, so normalized targets may
    // extend beyond the [-1, 1] base range.
    return kBaseVerts[i] + aiVector3D(100.0f + static_cast<float>(i), 200.0f, 300.0f);
}

aiVector3D TargetNormal(unsigned i) {
    return (-kBaseNormals[i]).Normalize();
}

aiVector3D TargetUV2(unsigned i) {
    return aiVector3D(7.0f + static_cast<float>(i), 8.0f, 9.0f);
}

aiColor4D TargetColor1(unsigned i) {
    return aiColor4D(0.5f + 0.1f * static_cast<float>(i), 0.6f, 0.7f, 1.0f);
}

aiAnimMesh *BuildAnimMesh(const MorphSpec &spec) {
    auto *m = new aiAnimMesh();
    m->mName.Set(spec.name);
    m->mWeight = spec.weight;
    m->mNumVertices = kNumVerts;

    if (spec.positions) {
        m->mVertices = new aiVector3D[kNumVerts];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            m->mVertices[i] = TargetPosition(i);
        }
    }
    if (spec.normals) {
        m->mNormals = new aiVector3D[kNumVerts];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            m->mNormals[i] = TargetNormal(i);
        }
    }
    if (spec.tangents) {
        m->mTangents = new aiVector3D[kNumVerts];
        m->mBitangents = new aiVector3D[kNumVerts];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            m->mTangents[i] = aiVector3D(0.0f, 0.0f, -1.0f);
            m->mBitangents[i] = aiVector3D(-1.0f, 0.0f, 0.0f);
        }
    }
    if (spec.uv2) {
        m->mTextureCoords[2] = new aiVector3D[kNumVerts];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            m->mTextureCoords[2][i] = TargetUV2(i);
        }
    }
    if (spec.color1) {
        m->mColors[1] = new aiColor4D[kNumVerts];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            m->mColors[1][i] = TargetColor1(i);
        }
    }
    return m;
}

// Build a mesh with normals, tangents, sparse UV slots (0 and 2) and a
// sparse color slot (1), plus the requested morph targets.
aiMesh *BuildMesh(const MorphSpec *specs, unsigned numSpecs,
        aiMorphingMethod method = aiMorphingMethod_VERTEX_BLEND) {
    auto *m = new aiMesh();
    m->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    m->mMaterialIndex = 0;
    m->mMethod = method;
    m->mNumVertices = kNumVerts;

    m->mVertices = new aiVector3D[kNumVerts];
    m->mNormals = new aiVector3D[kNumVerts];
    m->mTangents = new aiVector3D[kNumVerts];
    m->mBitangents = new aiVector3D[kNumVerts];
    for (unsigned i = 0; i < kNumVerts; ++i) {
        m->mVertices[i] = kBaseVerts[i];
        m->mNormals[i] = kBaseNormals[i];
        m->mTangents[i] = aiVector3D(1.0f, 0.0f, 0.0f);
        m->mBitangents[i] = aiVector3D(0.0f, 1.0f, 0.0f);
    }

    // Non-contiguous UV slots: slot 0 and slot 2, slot 1 stays null.
    m->mTextureCoords[0] = new aiVector3D[kNumVerts];
    m->mTextureCoords[2] = new aiVector3D[kNumVerts];
    m->mNumUVComponents[0] = 2;
    m->mNumUVComponents[2] = 3;
    // Non-contiguous color slot: slot 1 only, slot 0 stays null.
    m->mColors[1] = new aiColor4D[kNumVerts];
    for (unsigned i = 0; i < kNumVerts; ++i) {
        m->mTextureCoords[0][i] = aiVector3D(static_cast<float>(i), i + 0.5f, 0.0f);
        m->mTextureCoords[2][i] = aiVector3D(2.0f * i, 2.0f * i + 0.25f, 0.75f);
        m->mColors[1][i] = aiColor4D(0.1f * i, 0.2f, 0.3f, 1.0f);
    }

    m->mNumFaces = 2;
    m->mFaces = new aiFace[2];
    static const unsigned kIndices[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (unsigned f = 0; f < 2; ++f) {
        m->mFaces[f].mNumIndices = 3;
        m->mFaces[f].mIndices = new unsigned[3];
        std::memcpy(m->mFaces[f].mIndices, kIndices[f], 3 * sizeof(unsigned));
    }

    m->mNumAnimMeshes = numSpecs;
    m->mAnimMeshes = new aiAnimMesh *[numSpecs ? numSpecs : 1];
    for (unsigned i = 0; i < numSpecs; ++i) {
        m->mAnimMeshes[i] = BuildAnimMesh(specs[i]);
    }
    return m;
}

// Two target layouts used across the tests.
const MorphSpec kLayoutSmileFrown[2] = {
        { "smile", 0.25f, true, false, false, false, false },
        { "frown", 0.75f, false, true, false, false, false },
};

// Same ordered layout, but the replacement channels are complementary.
const MorphSpec kLayoutComplementary[2] = {
        { "smile", 0.25f, false, true, false, true, false },
        { "frown", 0.75f, true, false, false, false, true },
};

// Affine transform with non-uniform scale, a shear term and a
// translation. Assimp stores the translation in the fourth column
// (a4, b4, c4).
aiMatrix4x4 ParentMatrix() {
    // columns: (2,0,0), (0,3,0), (1,0,1), translation (10,20,30)
    return aiMatrix4x4(
            2.0f, 0.0f, 0.0f, 10.0f,
            0.0f, 3.0f, 0.0f, 20.0f,
            1.0f, 0.0f, 1.0f, 30.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
}

aiMatrix4x4 ChildMatrix() {
    // local child transform: non-uniform scale plus translation
    return aiMatrix4x4(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 7.0f,
            0.0f, 0.0f, 2.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
}

aiMatrix4x4 ReflectionMatrix() {
    return aiMatrix4x4(
            -1.0f, 0.0f, 0.0f, 4.0f,
            0.0f, 1.0f, 0.0f, 5.0f,
            0.0f, 0.0f, 1.0f, 6.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
}

// Scene with one root node and one child node, each referencing the
// meshes given by index.
aiScene *BuildTwoNodeScene(const std::vector<aiMesh *> &meshes,
        unsigned rootMesh, unsigned childMesh,
        const aiMatrix4x4 &rootTransform, const aiMatrix4x4 &childTransform) {
    auto *scene = new aiScene();
    scene->mNumMeshes = static_cast<unsigned>(meshes.size());
    scene->mMeshes = new aiMesh *[meshes.size()];
    for (size_t i = 0; i < meshes.size(); ++i) {
        scene->mMeshes[i] = meshes[i];
    }
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1] { new aiMaterial() };

    auto *root = new aiNode();
    root->mName.Set("root");
    root->mTransformation = rootTransform;
    root->mNumMeshes = 1;
    root->mMeshes = new unsigned[1] { rootMesh };

    auto *child = new aiNode();
    child->mName.Set("child");
    child->mTransformation = childTransform;
    child->mParent = root;
    child->mNumMeshes = 1;
    child->mMeshes = new unsigned[1] { childMesh };

    root->mNumChildren = 1;
    root->mChildren = new aiNode *[1] { child };
    scene->mRootNode = root;
    return scene;
}

void RunPretransform(aiScene *scene, bool keepHierarchy, bool preserveMorphs,
        bool normalize = false) {
    Assimp::Importer importer;
    if (preserveMorphs) {
        importer.SetPropertyBool(AI_CONFIG_PP_PTV_PRESERVE_MORPHS, true);
    }
    if (normalize) {
        importer.SetPropertyBool(AI_CONFIG_PP_PTV_NORMALIZE, true);
    }
    Assimp::PretransformVertices step;
    step.SetupProperties(&importer);
    step.KeepHierarchy(keepHierarchy);
    step.Execute(scene);
}

// Take ownership of a scene's mesh so it outlives the scene.
aiMesh *StealMesh(aiScene *scene, unsigned index) {
    aiMesh *mesh = scene->mMeshes[index];
    scene->mMeshes[index] = nullptr;
    return mesh;
}

// Check the baked base streams of one segment.
int CheckBakedBase(const aiMesh *mesh, const aiMatrix4x4 &mat,
        unsigned offset, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const unsigned o = offset + i;
        TASK_CHECK(NearV(mesh->mVertices[o], RefPosition(mat, kBaseVerts[i])));
        TASK_CHECK(NearV(mesh->mNormals[o], RefDirection(mat, kBaseNormals[i])));
        TASK_CHECK(NearV(mesh->mTangents[o],
                RefDirection(mat, aiVector3D(1.0f, 0.0f, 0.0f))));
        TASK_CHECK(NearV(mesh->mBitangents[o],
                RefDirection(mat, aiVector3D(0.0f, 1.0f, 0.0f))));
        // UVs and colors are not spatially transformed.
        TASK_CHECK(NearV(mesh->mTextureCoords[0][o],
                aiVector3D(static_cast<float>(i), i + 0.5f, 0.0f)));
        TASK_CHECK(NearV(mesh->mTextureCoords[2][o],
                aiVector3D(2.0f * i, 2.0f * i + 0.25f, 0.75f)));
        TASK_CHECK(NearC(mesh->mColors[1][o], aiColor4D(0.1f * i, 0.2f, 0.3f, 1.0f)));
    }
    TASK_CHECK(mesh->mTextureCoords[1] == nullptr);
    TASK_CHECK(mesh->mColors[0] == nullptr);
    return 0;
}

// ---------------------------------------------------------------------------
// instances group
// ---------------------------------------------------------------------------

// One source mesh referenced by a parent and its child with different
// accumulated matrices, keep-hierarchy entry point.
int InstanceKeepSharedSource() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());

    RunPretransform(scene, true, true);

    const aiMatrix4x4 rootMat = ParentMatrix();
    const aiMatrix4x4 childMat = ParentMatrix() * ChildMatrix();

    TASK_CHECK(scene->mNumMeshes == 2);
    aiMesh *rootBaked = nullptr;
    aiMesh *childBaked = nullptr;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        if (NearV(scene->mMeshes[i]->mVertices[0], RefPosition(rootMat, kBaseVerts[0]))) {
            rootBaked = scene->mMeshes[i];
        } else {
            childBaked = scene->mMeshes[i];
        }
    }
    TASK_CHECK(rootBaked != nullptr);
    TASK_CHECK(childBaked != nullptr);
    TASK_CHECK(rootBaked != childBaked);

    TASK_CHECK(CheckBakedBase(rootBaked, rootMat, 0, kNumVerts) == 0);
    TASK_CHECK(CheckBakedBase(childBaked, childMat, 0, kNumVerts) == 0);

    for (aiMesh *baked : { rootBaked, childBaked }) {
        const aiMatrix4x4 &mat = (baked == rootBaked) ? rootMat : childMat;
        TASK_CHECK(baked->mNumAnimMeshes == 2);
        TASK_CHECK(baked->mMethod == aiMorphingMethod_VERTEX_BLEND);
        const aiAnimMesh *smile = baked->mAnimMeshes[0];
        const aiAnimMesh *frown = baked->mAnimMeshes[1];
        TASK_CHECK(std::strcmp(smile->mName.C_Str(), "smile") == 0);
        TASK_CHECK(Near(smile->mWeight, 0.25f));
        TASK_CHECK(std::strcmp(frown->mName.C_Str(), "frown") == 0);
        TASK_CHECK(Near(frown->mWeight, 0.75f));

        TASK_CHECK(smile->mVertices != nullptr);
        TASK_CHECK(smile->mNormals == nullptr);
        for (unsigned i = 0; i < kNumVerts; ++i) {
            // Absolute replacement stream transformed with the full matrix.
            TASK_CHECK(NearV(smile->mVertices[i], RefPosition(mat, TargetPosition(i))));
        }
        TASK_CHECK(frown->mVertices == nullptr);
        TASK_CHECK(frown->mNormals != nullptr);
        for (unsigned i = 0; i < kNumVerts; ++i) {
            TASK_CHECK(NearV(frown->mNormals[i], RefDirection(mat, TargetNormal(i))));
        }
    }

    // Independent ownership: the two instances must not share arrays.
    TASK_CHECK(rootBaked->mVertices != childBaked->mVertices);
    TASK_CHECK(rootBaked->mAnimMeshes[0]->mVertices != childBaked->mAnimMeshes[0]->mVertices);

    aiMesh *keptRoot = StealMesh(scene, 0);
    aiMesh *keptChild = StealMesh(scene, 1);
    scene->mNumMeshes = 0;
    delete scene;
    delete keptChild;
    // Deleting one instance must not corrupt the other one.
    TASK_CHECK(keptRoot->mNumAnimMeshes == 2);
    TASK_CHECK(keptRoot->mAnimMeshes[0]->mVertices != nullptr);
    delete keptRoot;
    return 0;
}

// Same shared source through the collapse entry point.
int InstanceCollapseSharedSource() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());

    RunPretransform(scene, false, true);

    const aiMatrix4x4 rootMat = ParentMatrix();
    const aiMatrix4x4 childMat = ParentMatrix() * ChildMatrix();

    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *merged = scene->mMeshes[0];
    TASK_CHECK(merged->mNumVertices == 2 * kNumVerts);
    TASK_CHECK(merged->mNumFaces == 4);
    TASK_CHECK(merged->mNumAnimMeshes == 2);

    TASK_CHECK(CheckBakedBase(merged, rootMat, 0, kNumVerts) == 0);
    TASK_CHECK(CheckBakedBase(merged, childMat, kNumVerts, kNumVerts) == 0);

    // Face indices of the second segment are offset onto its vertices.
    TASK_CHECK(merged->mFaces[2].mIndices[0] == kNumVerts);
    TASK_CHECK(merged->mFaces[3].mIndices[2] == kNumVerts + 3);

    const aiAnimMesh *smile = merged->mAnimMeshes[0];
    TASK_CHECK(smile->mNumVertices == 2 * kNumVerts);
    TASK_CHECK(smile->mVertices != nullptr);
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(smile->mVertices[i], RefPosition(rootMat, TargetPosition(i))));
        TASK_CHECK(NearV(smile->mVertices[kNumVerts + i],
                RefPosition(childMat, TargetPosition(i))));
    }
    // The frown target only carries normals; positions inherit the
    // transformed base of each segment.
    const aiAnimMesh *frown = merged->mAnimMeshes[1];
    TASK_CHECK(frown->mVertices == nullptr);
    TASK_CHECK(frown->mNormals != nullptr);
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(frown->mNormals[i], RefDirection(rootMat, TargetNormal(i))));
        TASK_CHECK(NearV(frown->mNormals[kNumVerts + i],
                RefDirection(childMat, TargetNormal(i))));
    }

    delete scene;
    return 0;
}

// Two different meshes with complementary missing target channels.
int InstanceCollapseComplementaryChannels() {
    aiMesh *meshA = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *meshB = BuildMesh(kLayoutComplementary, 2);
    aiScene *scene = BuildTwoNodeScene({ meshA, meshB }, 0, 1,
            ParentMatrix(), ChildMatrix());

    RunPretransform(scene, false, true);

    const aiMatrix4x4 rootMat = ParentMatrix();
    const aiMatrix4x4 childMat = ParentMatrix() * ChildMatrix();

    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *merged = scene->mMeshes[0];
    TASK_CHECK(merged->mNumAnimMeshes == 2);

    const aiAnimMesh *smile = merged->mAnimMeshes[0];
    const aiAnimMesh *frown = merged->mAnimMeshes[1];

    // Union of the replacement channels; tangents exist on neither.
    TASK_CHECK(smile->mVertices != nullptr);
    TASK_CHECK(smile->mNormals != nullptr);
    TASK_CHECK(smile->mTangents == nullptr);
    TASK_CHECK(smile->mBitangents == nullptr);
    TASK_CHECK(smile->mTextureCoords[2] != nullptr);
    TASK_CHECK(frown->mVertices != nullptr);
    TASK_CHECK(frown->mNormals != nullptr);
    TASK_CHECK(frown->mColors[1] != nullptr);

    // Segment A: smile positions replaced, smile normals and UV2
    // inherited from the already transformed A base (UV streams are not
    // spatially transformed, so the base UV values pass through).
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(smile->mVertices[i], RefPosition(rootMat, TargetPosition(i))));
        TASK_CHECK(NearV(smile->mNormals[i], RefDirection(rootMat, kBaseNormals[i])));
        TASK_CHECK(NearV(smile->mTextureCoords[2][i],
                aiVector3D(2.0f * i, 2.0f * i + 0.25f, 0.75f)));
    }
    // Segment B: smile positions inherited from the B base, normals and
    // UV2 replaced by the target stream.
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(smile->mVertices[kNumVerts + i], RefPosition(childMat, kBaseVerts[i])));
        TASK_CHECK(NearV(smile->mNormals[kNumVerts + i],
                RefDirection(childMat, TargetNormal(i))));
        TASK_CHECK(NearV(smile->mTextureCoords[2][kNumVerts + i], TargetUV2(i)));
    }
    // frown: A normals replaced, B positions replaced; B color1 replaced
    // while A inherits the transformed A base color slot.
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(frown->mVertices[i], RefPosition(rootMat, kBaseVerts[i])));
        TASK_CHECK(NearV(frown->mNormals[i], RefDirection(rootMat, TargetNormal(i))));
        TASK_CHECK(NearC(frown->mColors[1][i], aiColor4D(0.1f * i, 0.2f, 0.3f, 1.0f)));
        TASK_CHECK(NearV(frown->mVertices[kNumVerts + i],
                RefPosition(childMat, TargetPosition(i))));
        TASK_CHECK(NearV(frown->mNormals[kNumVerts + i],
                RefDirection(childMat, kBaseNormals[i])));
        TASK_CHECK(NearC(frown->mColors[1][kNumVerts + i], TargetColor1(i)));
    }

    // A channel nobody provides stays a null replacement array.
    TASK_CHECK(smile->mColors[0] == nullptr);
    TASK_CHECK(frown->mTextureCoords[0] == nullptr);

    delete scene;
    return 0;
}

// Non-contiguous UV and color slots survive both hierarchy entries.
int InstanceSparseSlots() {
    for (int keep = 0; keep <= 1; ++keep) {
        aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
        aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());
        RunPretransform(scene, keep != 0, true);

        for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
            const aiMesh *baked = scene->mMeshes[m];
            TASK_CHECK(baked->mTextureCoords[0] != nullptr);
            TASK_CHECK(baked->mTextureCoords[1] == nullptr);
            TASK_CHECK(baked->mTextureCoords[2] != nullptr);
            TASK_CHECK(baked->mNumUVComponents[2] == 3);
            TASK_CHECK(baked->mColors[0] == nullptr);
            TASK_CHECK(baked->mColors[1] != nullptr);

            for (unsigned t = 0; t < baked->mNumAnimMeshes; ++t) {
                const aiAnimMesh *morph = baked->mAnimMeshes[t];
                TASK_CHECK(morph->mTextureCoords[0] == nullptr);
                TASK_CHECK(morph->mTextureCoords[1] == nullptr);
                TASK_CHECK(morph->mColors[0] == nullptr);
            }
        }
        delete scene;
    }
    return 0;
}

// Reflection flips the base face winding but keeps the per-vertex
// correspondence of every morph stream.
int InstanceReflectionWinding() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    const aiMatrix4x4 reflect = ReflectionMatrix();
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, reflect, aiMatrix4x4());

    RunPretransform(scene, true, true);

    TASK_CHECK(scene->mNumMeshes >= 1);
    const aiMesh *baked = scene->mMeshes[0];
    TASK_CHECK(baked->mFaces[0].mIndices[0] == 2);
    TASK_CHECK(baked->mFaces[0].mIndices[1] == 1);
    TASK_CHECK(baked->mFaces[0].mIndices[2] == 0);

    const aiAnimMesh *smile = baked->mAnimMeshes[0];
    TASK_CHECK(smile->mVertices != nullptr);
    for (unsigned i = 0; i < kNumVerts; ++i) {
        // Not reordered: vertex i still matches source vertex i.
        TASK_CHECK(NearV(smile->mVertices[i], RefPosition(reflect, TargetPosition(i))));
        TASK_CHECK(NearV(baked->mVertices[i], RefPosition(reflect, kBaseVerts[i])));
    }
    const aiAnimMesh *frown = baked->mAnimMeshes[1];
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(frown->mNormals[i], RefDirection(reflect, TargetNormal(i))));
    }

    delete scene;
    return 0;
}

// With the option off, the legacy behavior is unchanged: targets keep
// their original coordinates in keep mode and vanish in collapse mode.
int InstanceDisabledKeepsLegacyBehavior() {
    aiMesh *keepSource = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *keepScene = BuildTwoNodeScene({ keepSource }, 0, 0,
            ParentMatrix(), ChildMatrix());
    RunPretransform(keepScene, true, false);

    for (unsigned m = 0; m < keepScene->mNumMeshes; ++m) {
        const aiMesh *baked = keepScene->mMeshes[m];
        TASK_CHECK(baked->mNumAnimMeshes == 2);
        // Untouched local coordinates.
        TASK_CHECK(NearV(baked->mAnimMeshes[0]->mVertices[0], TargetPosition(0)));
    }
    delete keepScene;

    aiMesh *collapseSource = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *collapseScene = BuildTwoNodeScene({ collapseSource }, 0, 0,
            ParentMatrix(), ChildMatrix());
    RunPretransform(collapseScene, false, false);
    TASK_CHECK(collapseScene->mNumMeshes == 1);
    TASK_CHECK(collapseScene->mMeshes[0]->mNumAnimMeshes == 0);
    TASK_CHECK(collapseScene->mMeshes[0]->mAnimMeshes == nullptr);
    delete collapseScene;
    return 0;
}

// Animations are still removed and node matrices still follow the
// original pretransform flow.
int InstanceHierarchySideEffects() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *keepScene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());
    RunPretransform(keepScene, true, true);
    TASK_CHECK(keepScene->mNumAnimations == 0);
    TASK_CHECK(keepScene->mRootNode->mTransformation.IsIdentity());
    TASK_CHECK(keepScene->mRootNode->mChildren[0]->mTransformation.IsIdentity());
    delete keepScene;

    aiMesh *source2 = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *collapseScene = BuildTwoNodeScene({ source2 }, 0, 0, ParentMatrix(), ChildMatrix());
    RunPretransform(collapseScene, false, true);
    TASK_CHECK(collapseScene->mNumAnimations == 0);
    TASK_CHECK(collapseScene->mRootNode->mTransformation.IsIdentity());
    delete collapseScene;
    return 0;
}

// Identity node matrices leave every morph stream exactly as authored.
int InstanceIdentityTransform() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, aiMatrix4x4(), aiMatrix4x4());
    RunPretransform(scene, false, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *merged = scene->mMeshes[0];
    TASK_CHECK(merged->mNumVertices == 2 * kNumVerts);
    const aiAnimMesh *smile = merged->mAnimMeshes[0];
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(smile->mVertices[i], TargetPosition(i)));
        TASK_CHECK(NearV(smile->mVertices[kNumVerts + i], TargetPosition(i)));
    }
    delete scene;
    return 0;
}

// Tangent and bitangent replacement streams use the inverse-transpose
// rule and are normalized, just like the base mesh directions.
int InstanceTargetTangents() {
    const MorphSpec tangentSpec[1] = {
            { "grin", 0.4f, true, false, true, false, false },
    };
    aiMesh *source = BuildMesh(tangentSpec, 1);
    const aiMatrix4x4 mat = ParentMatrix();
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, mat, aiMatrix4x4());
    RunPretransform(scene, true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *baked = scene->mMeshes[0];
    TASK_CHECK(baked->mNumAnimMeshes == 1);
    const aiAnimMesh *grin = baked->mAnimMeshes[0];
    TASK_CHECK(std::strcmp(grin->mName.C_Str(), "grin") == 0);
    TASK_CHECK(Near(grin->mWeight, 0.4f));
    TASK_CHECK(grin->mTangents != nullptr);
    TASK_CHECK(grin->mBitangents != nullptr);
    TASK_CHECK(grin->mNormals == nullptr);

    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(grin->mVertices[i], RefPosition(mat, TargetPosition(i))));
        TASK_CHECK(NearV(grin->mTangents[i],
                RefDirection(mat, aiVector3D(0.0f, 0.0f, -1.0f))));
        TASK_CHECK(NearV(grin->mBitangents[i],
                RefDirection(mat, aiVector3D(-1.0f, 0.0f, 0.0f))));
    }

    delete scene;
    return 0;
}

// Same material but different ordered target layouts stay in separate
// output meshes when the hierarchy is collapsed.
int InstanceDistinctLayoutsStaySeparate() {
    const MorphSpec otherLayout[2] = {
            { "smile", 0.25f, true, false, false, false, false },
            { "angry", 0.75f, false, true, false, false, false },
    };
    aiMesh *meshA = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *meshB = BuildMesh(otherLayout, 2);
    TASK_CHECK(meshA->mMaterialIndex == meshB->mMaterialIndex);
    aiScene *scene = BuildTwoNodeScene({ meshA, meshB }, 0, 1,
            ParentMatrix(), ChildMatrix());
    RunPretransform(scene, false, true);

    TASK_CHECK(scene->mNumMeshes == 2);
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *baked = scene->mMeshes[m];
        TASK_CHECK(baked->mNumVertices == kNumVerts);
        TASK_CHECK(baked->mNumAnimMeshes == 2);
        TASK_CHECK(baked->mAnimMeshes[1] != nullptr);
        // Each output keeps the layout of its own source.
        const bool isFrown = std::strcmp(baked->mAnimMeshes[1]->mName.C_Str(), "frown") == 0;
        const bool isAngry = std::strcmp(baked->mAnimMeshes[1]->mName.C_Str(), "angry") == 0;
        TASK_CHECK(isFrown != isAngry);
        TASK_CHECK(baked->mAnimMeshes[0]->mVertices != nullptr);
    }

    delete scene;
    return 0;
}

// ---------------------------------------------------------------------------
// merge group (direct SceneCombiner::MergeMeshes usage)
// ---------------------------------------------------------------------------

int MergeStitchUnion() {
    aiMesh *a = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *b = BuildMesh(kLayoutComplementary, 2);
    std::vector<aiMesh *> in { a, b };

    // Snapshot an input value for the "inputs are not modified" check.
    const aiVector3D snapshot = a->mAnimMeshes[0]->mVertices[0];
    const unsigned faceSnapshot = a->mFaces[0].mIndices[0];

    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            in.begin(), in.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    TASK_CHECK(out->mNumVertices == 2 * kNumVerts);
    TASK_CHECK(out->mNumFaces == 4);
    TASK_CHECK(out->mNumAnimMeshes == 2);
    TASK_CHECK(out->mMethod == aiMorphingMethod_VERTEX_BLEND);

    const aiAnimMesh *smile = out->mAnimMeshes[0];
    const aiAnimMesh *frown = out->mAnimMeshes[1];
    TASK_CHECK(std::strcmp(smile->mName.C_Str(), "smile") == 0);
    TASK_CHECK(Near(smile->mWeight, 0.25f));
    TASK_CHECK(std::strcmp(frown->mName.C_Str(), "frown") == 0);
    TASK_CHECK(Near(frown->mWeight, 0.75f));

    // No spatial transform is applied: identity segments, values copied.
    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(smile->mVertices[i], TargetPosition(i)));
        TASK_CHECK(NearV(smile->mNormals[i], kBaseNormals[i]));
        TASK_CHECK(NearV(smile->mNormals[kNumVerts + i], TargetNormal(i)));
        TASK_CHECK(NearV(smile->mVertices[kNumVerts + i], kBaseVerts[i]));
        TASK_CHECK(NearV(frown->mVertices[i], kBaseVerts[i]));
        TASK_CHECK(NearV(frown->mNormals[i], TargetNormal(i)));
        TASK_CHECK(NearV(frown->mVertices[kNumVerts + i], TargetPosition(i)));
    }
    // Tangents are absent from every input target -> remain absent.
    TASK_CHECK(smile->mTangents == nullptr);
    TASK_CHECK(frown->mBitangents == nullptr);

    // Face indices keep per-segment correspondence and are offset.
    TASK_CHECK(out->mFaces[0].mIndices[0] == 0);
    TASK_CHECK(out->mFaces[2].mIndices[0] == kNumVerts);

    // The inputs were not modified and still own their data.
    TASK_CHECK(NearV(a->mAnimMeshes[0]->mVertices[0], snapshot));
    TASK_CHECK(a->mFaces[0].mIndices[0] == faceSnapshot);
    TASK_CHECK(a->mNumAnimMeshes == 2);
    TASK_CHECK(b->mNumAnimMeshes == 2);

    out.reset();
    // Output destruction leaves the inputs fully functional.
    TASK_CHECK(NearV(a->mAnimMeshes[0]->mVertices[0], snapshot));
    TASK_CHECK(NearV(b->mAnimMeshes[0]->mNormals[0], TargetNormal(0)));
    TASK_CHECK(a->mFaces[1].mIndices[2] == 3);
    delete a;
    delete b;
    return 0;
}

int MergeSparseSlots() {
    aiMesh *a = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *b = BuildMesh(kLayoutSmileFrown, 2);
    std::vector<aiMesh *> in { a, b };

    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            in.begin(), in.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out->mTextureCoords[0] != nullptr);
    TASK_CHECK(out->mTextureCoords[1] == nullptr);
    TASK_CHECK(out->mTextureCoords[2] != nullptr);
    TASK_CHECK(out->mNumUVComponents[2] == 3);
    TASK_CHECK(out->mColors[0] == nullptr);
    TASK_CHECK(out->mColors[1] != nullptr);
    TASK_CHECK(out->mNumVertices == 2 * kNumVerts);
    TASK_CHECK(NearV(out->mTextureCoords[2][kNumVerts + 1],
            aiVector3D(2.0f, 2.25f, 0.75f)));

    out.reset();
    delete a;
    delete b;
    return 0;
}

int MergeIncompatibleLayouts() {
    struct Variant {
        MorphSpec specs[2];
        aiMorphingMethod method;
    };
    const Variant variants[] = {
            // different target name
            { { { "smile", 0.25f, true, false, false, false, false },
                    { "angry", 0.75f, false, true, false, false, false } },
                    aiMorphingMethod_VERTEX_BLEND },
            // different weight
            { { { "smile", 0.25f, true, false, false, false, false },
                    { "frown", 0.50f, false, true, false, false, false } },
                    aiMorphingMethod_VERTEX_BLEND },
            // different morph method
            { { kLayoutSmileFrown[0], kLayoutSmileFrown[1] },
                    aiMorphingMethod_MORPH_RELATIVE },
    };

    for (const Variant &variant : variants) {
        aiMesh *a = BuildMesh(kLayoutSmileFrown, 2);
        aiMesh *b = BuildMesh(variant.specs, 2, variant.method);
        std::vector<aiMesh *> in { a, b };

        bool threw = false;
        aiMesh *raw = nullptr;
        try {
            Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                    in.begin(), in.end());
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(raw == nullptr);
        // A rejected merge does not consume the inputs.
        TASK_CHECK(a->mNumAnimMeshes == 2);
        TASK_CHECK(b->mNumAnimMeshes == 2);
        delete a;
        delete b;
    }

    // No targets versus targets.
    aiMesh *plain = BuildMesh(nullptr, 0);
    aiMesh *withTargets = BuildMesh(kLayoutSmileFrown, 2);
    std::vector<aiMesh *> in { plain, withTargets };
    bool threw = false;
    aiMesh *raw = nullptr;
    try {
        Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                in.begin(), in.end());
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    delete plain;
    delete withTargets;
    return 0;
}

int MergeEmptyRange() {
    std::vector<aiMesh *> in;
    aiMesh *raw = reinterpret_cast<aiMesh *>(0x1);
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            in.begin(), in.end());
    TASK_CHECK(raw == nullptr);
    return 0;
}

int MergeFlagsZeroLegacy() {
    aiMesh *a = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *b = BuildMesh(kLayoutSmileFrown, 2);
    std::vector<aiMesh *> in { a, b };

    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, 0, in.begin(), in.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    TASK_CHECK(out->mNumVertices == 2 * kNumVerts);
    // Legacy contract: morph targets are dropped and inputs are consumed.
    TASK_CHECK(out->mNumAnimMeshes == 0);
    TASK_CHECK(out->mAnimMeshes == nullptr);
    return 0;
}

int MergeIndependentLifecycle() {
    aiMesh *a = BuildMesh(kLayoutSmileFrown, 2);
    aiMesh *b = BuildMesh(kLayoutSmileFrown, 2);
    std::vector<aiMesh *> in { a, b };

    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            in.begin(), in.end());
    aiMesh *out = raw;

    TASK_CHECK(out->mAnimMeshes[0]->mVertices != a->mAnimMeshes[0]->mVertices);
    TASK_CHECK(out->mAnimMeshes[0]->mVertices != b->mAnimMeshes[0]->mVertices);
    TASK_CHECK(out->mVertices != a->mVertices);
    TASK_CHECK(out->mFaces[0].mIndices != a->mFaces[0].mIndices);

    delete out;
    TASK_CHECK(a->mNumVertices == kNumVerts);
    TASK_CHECK(b->mAnimMeshes[1]->mNormals != nullptr);
    delete a;
    TASK_CHECK(b->mNumVertices == kNumVerts);
    delete b;
    return 0;
}

// ---------------------------------------------------------------------------
// normalize group
// ---------------------------------------------------------------------------

// The step derives one map from the bounding box of ALL baked base
// meshes: a scene-wide center and a dominant half-extent.
void ComputeGlobalNormalizationMap(const std::vector<aiMatrix4x4> &mats,
        aiVector3D &center, float &div) {
    aiVector3D mn(1e30f, 1e30f, 1e30f);
    aiVector3D mx(-1e30f, -1e30f, -1e30f);
    for (const aiMatrix4x4 &mat : mats) {
        for (const aiVector3D &v : kBaseVerts) {
            const aiVector3D baked = RefPosition(mat, v);
            mn = aiVector3D(std::min(mn.x, baked.x), std::min(mn.y, baked.y), std::min(mn.z, baked.z));
            mx = aiVector3D(std::max(mx.x, baked.x), std::max(mx.y, baked.y), std::max(mx.z, baked.z));
        }
    }
    const aiVector3D extent = mx - mn;
    div = std::max(extent.x, std::max(extent.y, extent.z)) * 0.5f;
    center = mn + extent * 0.5f;
}

aiVector3D Normalized(const aiVector3D &baked, const aiVector3D &center, float div) {
    return (baked - center) / div;
}

int NormalizeCollapse() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());
    RunPretransform(scene, false, true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *merged = scene->mMeshes[0];

    const aiMatrix4x4 rootMat = ParentMatrix();
    const aiMatrix4x4 childMat = ParentMatrix() * ChildMatrix();
    aiVector3D center;
    float div;
    ComputeGlobalNormalizationMap({ rootMat, childMat }, center, div);

    for (unsigned i = 0; i < kNumVerts; ++i) {
        TASK_CHECK(NearV(merged->mVertices[i],
                Normalized(RefPosition(rootMat, kBaseVerts[i]), center, div)));
        TASK_CHECK(NearV(merged->mVertices[kNumVerts + i],
                Normalized(RefPosition(childMat, kBaseVerts[i]), center, div)));
    }

    // Same map applied to target positions; targets are allowed outside
    // the normalized base range.
    const aiAnimMesh *smile = merged->mAnimMeshes[0];
    TASK_CHECK(smile->mVertices != nullptr);
    bool outsideRange = false;
    for (unsigned i = 0; i < kNumVerts; ++i) {
        const aiVector3D expected = Normalized(RefPosition(rootMat, TargetPosition(i)), center, div);
        TASK_CHECK(NearV(smile->mVertices[i], expected));
        if (std::fabs(expected.x) > 1.0f || std::fabs(expected.y) > 1.0f ||
                std::fabs(expected.z) > 1.0f) {
            outsideRange = true;
        }
    }
    TASK_CHECK(outsideRange);

    delete scene;
    return 0;
}

int NormalizeKeep() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), ChildMatrix());
    RunPretransform(scene, true, true, true);

    const aiMatrix4x4 rootMat = ParentMatrix();
    const aiMatrix4x4 childMat = ParentMatrix() * ChildMatrix();
    aiVector3D center;
    float div;
    ComputeGlobalNormalizationMap({ rootMat, childMat }, center, div);

    TASK_CHECK(scene->mNumMeshes == 2);
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *baked = scene->mMeshes[m];
        const bool isRoot = NearV(baked->mVertices[0],
                Normalized(RefPosition(rootMat, kBaseVerts[0]), center, div));
        const aiMatrix4x4 &mat = isRoot ? rootMat : childMat;

        for (unsigned i = 0; i < kNumVerts; ++i) {
            TASK_CHECK(NearV(baked->mVertices[i],
                    Normalized(RefPosition(mat, kBaseVerts[i]), center, div)));
        }
        const aiAnimMesh *smile = baked->mAnimMeshes[0];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            TASK_CHECK(NearV(smile->mVertices[i],
                    Normalized(RefPosition(mat, TargetPosition(i)), center, div)));
        }
        // Direction streams keep the plain inverse-transpose rule; the
        // normalization mapping must not be applied to them again.
        const aiAnimMesh *frown = baked->mAnimMeshes[1];
        for (unsigned i = 0; i < kNumVerts; ++i) {
            TASK_CHECK(NearV(frown->mNormals[i], RefDirection(mat, TargetNormal(i))));
        }
    }

    delete scene;
    return 0;
}

// The uniform scale must be identical for every axis and must be the
// same for base and morph targets (no per-target normalization).
int NormalizeUniformMapping() {
    aiMesh *source = BuildMesh(kLayoutSmileFrown, 2);
    aiScene *scene = BuildTwoNodeScene({ source }, 0, 0, ParentMatrix(), aiMatrix4x4());
    RunPretransform(scene, false, true, true);

    const aiMatrix4x4 mat = ParentMatrix();
    aiVector3D center;
    float div;
    ComputeGlobalNormalizationMap({ mat }, center, div);
    TASK_CHECK(div > 0.0f);

    const aiMesh *merged = scene->mMeshes[0];
    const aiAnimMesh *smile = merged->mAnimMeshes[0];

    // A pure base-space offset between two points survives as the same
    // scaled offset in every output stream (uniform, axis-independent).
    // Vertices 2 and 0 differ on x and y, which under the shear has a
    // nonzero component on every output axis.
    const aiVector3D baseDeltaBaked = RefPosition(mat, kBaseVerts[2]) - RefPosition(mat, kBaseVerts[0]);
    const aiVector3D baseDeltaOut = merged->mVertices[2] - merged->mVertices[0];
    const aiVector3D targetDeltaBaked =
            RefPosition(mat, TargetPosition(2)) - RefPosition(mat, TargetPosition(0));
    const aiVector3D targetDeltaOut = smile->mVertices[2] - smile->mVertices[0];

    for (int axis = 0; axis < 3; ++axis) {
        const float baseScale = baseDeltaOut[axis] / baseDeltaBaked[axis];
        const float targetScale = targetDeltaOut[axis] / targetDeltaBaked[axis];
        TASK_CHECK(Near(baseScale, 1.0f / div));
        TASK_CHECK(Near(targetScale, 1.0f / div));
        TASK_CHECK(Near(baseScale, targetScale));
    }

    delete scene;
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
        { "instances", "keep_shared_source", InstanceKeepSharedSource },
        { "instances", "collapse_shared_source", InstanceCollapseSharedSource },
        { "instances", "collapse_complementary_channels", InstanceCollapseComplementaryChannels },
        { "instances", "sparse_slots", InstanceSparseSlots },
        { "instances", "reflection_winding", InstanceReflectionWinding },
        { "instances", "disabled_legacy_behavior", InstanceDisabledKeepsLegacyBehavior },
        { "instances", "hierarchy_side_effects", InstanceHierarchySideEffects },
        { "instances", "identity_transform", InstanceIdentityTransform },
        { "instances", "target_tangents", InstanceTargetTangents },
        { "instances", "distinct_layouts_separate", InstanceDistinctLayoutsStaySeparate },

        { "merge", "stitch_union", MergeStitchUnion },
        { "merge", "sparse_slots", MergeSparseSlots },
        { "merge", "incompatible_layouts", MergeIncompatibleLayouts },
        { "merge", "empty_range", MergeEmptyRange },
        { "merge", "flags_zero_legacy", MergeFlagsZeroLegacy },
        { "merge", "independent_lifecycle", MergeIndependentLifecycle },

        { "normalize", "collapse_mapping", NormalizeCollapse },
        { "normalize", "keep_mapping", NormalizeKeep },
        { "normalize", "uniform_mapping", NormalizeUniformMapping },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
