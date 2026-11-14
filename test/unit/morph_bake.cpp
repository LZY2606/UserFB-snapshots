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
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

#include "task_support/check.h"

#include "PostProcessing/PretransformVertices.h"

#include <assimp/Importer.hpp>
#include <assimp/SceneCombiner.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include <assimp/types.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

bool NearV(const aiVector3D &a, const aiVector3D &b) {
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

bool VecArrayNear(const aiVector3D *actual, const std::vector<aiVector3D> &expected) {
    if (actual == nullptr) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!NearV(actual[i], expected[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// A deterministic affine transform: non-uniform scale, shear and translation.
aiMatrix4x4 ShearScaleTranslate() {
    aiMatrix4x4 m;
    m.a1 = 2.0f; m.a2 = 0.5f; m.a4 = 10.0f;
    m.b2 = 3.0f; m.b4 = 20.0f;
    m.c3 = 4.0f; m.c4 = 30.0f;
    return m;
}

// A second transform, used as a child of the first.
aiMatrix4x4 ChildTransform() {
    aiMatrix4x4 m;
    m.a1 = 1.0f; m.a2 = 0.25f; m.a4 = 5.0f;
    m.b2 = 0.5f; m.b3 = 0.5f; m.b4 = 6.0f;
    m.c3 = 2.0f; m.c4 = 7.0f;
    return m;
}

// A transform with a negative determinant (reflection).
aiMatrix4x4 ReflectionTransform() {
    aiMatrix4x4 m;
    m.a1 = 1.0f; m.a4 = 1.0f;
    m.b2 = 1.0f; m.b4 = 2.0f;
    m.c3 = -1.0f; m.c4 = 3.0f;
    return m;
}

std::vector<aiVector3D> BakedPositions(const aiMatrix4x4 &m, const std::vector<aiVector3D> &in) {
    std::vector<aiVector3D> out;
    for (size_t i = 0; i < in.size(); ++i) out.push_back(m * in[i]);
    return out;
}

std::vector<aiVector3D> BakedNormals(const aiMatrix4x4 &m, const std::vector<aiVector3D> &in) {
    const aiMatrix3x3 it = aiMatrix3x3(m).Inverse().Transpose();
    std::vector<aiVector3D> out;
    for (size_t i = 0; i < in.size(); ++i) out.push_back((it * in[i]).Normalize());
    return out;
}

// ---------------------------------------------------------------------------
aiMesh *NewMesh(const std::vector<aiVector3D> &positions) {
    aiMesh *mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = static_cast<unsigned int>(positions.size());
    mesh->mVertices = new aiVector3D[mesh->mNumVertices];
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) mesh->mVertices[i] = positions[i];

    ai_assert(positions.size() >= 3 && positions.size() % 3 == 0);
    mesh->mNumFaces = static_cast<unsigned int>(positions.size() / 3);
    mesh->mFaces = new aiFace[mesh->mNumFaces];
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        mesh->mFaces[f].mNumIndices = 3;
        mesh->mFaces[f].mIndices = new unsigned int[3];
        mesh->mFaces[f].mIndices[0] = f * 3 + 0;
        mesh->mFaces[f].mIndices[1] = f * 3 + 1;
        mesh->mFaces[f].mIndices[2] = f * 3 + 2;
    }
    return mesh;
}

void SetNormals(aiMesh *mesh, const std::vector<aiVector3D> &normals) {
    mesh->mNormals = new aiVector3D[mesh->mNumVertices];
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) mesh->mNormals[i] = normals[i];
}

void SetChannel(aiMesh *mesh, unsigned int slot, const std::vector<aiVector3D> &uv) {
    ai_assert(slot < AI_MAX_NUMBER_OF_TEXTURECOORDS);
    mesh->mTextureCoords[slot] = new aiVector3D[mesh->mNumVertices];
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) mesh->mTextureCoords[slot][i] = uv[i];
    mesh->mNumUVComponents[slot] = 2;
}

void SetColorChannel(aiMesh *mesh, unsigned int slot, const std::vector<aiColor4D> &colors) {
    ai_assert(slot < AI_MAX_NUMBER_OF_COLOR_SETS);
    mesh->mColors[slot] = new aiColor4D[mesh->mNumVertices];
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) mesh->mColors[slot][i] = colors[i];
}

aiAnimMesh *AddTarget(aiMesh *mesh, const char *name, float weight) {
    aiAnimMesh **targets = new aiAnimMesh *[mesh->mNumAnimMeshes + 1];
    for (unsigned int i = 0; i < mesh->mNumAnimMeshes; ++i) targets[i] = mesh->mAnimMeshes[i];
    delete[] mesh->mAnimMeshes;
    mesh->mAnimMeshes = targets;

    aiAnimMesh *target = new aiAnimMesh();
    target->mName.Set(name);
    target->mWeight = weight;
    target->mNumVertices = mesh->mNumVertices;
    mesh->mAnimMeshes[mesh->mNumAnimMeshes++] = target;
    return target;
}

void SetTargetPositions(aiAnimMesh *target, const std::vector<aiVector3D> &values) {
    target->mVertices = new aiVector3D[target->mNumVertices];
    for (unsigned int i = 0; i < target->mNumVertices; ++i) target->mVertices[i] = values[i];
}

void SetTargetNormals(aiAnimMesh *target, const std::vector<aiVector3D> &values) {
    target->mNormals = new aiVector3D[target->mNumVertices];
    for (unsigned int i = 0; i < target->mNumVertices; ++i) target->mNormals[i] = values[i];
}

// ---------------------------------------------------------------------------
// Build a scene whose root references mesh @p rootMesh and, optionally, whose
// only child references @p childMesh. Both transforms are local.
aiScene *NewScene(std::vector<aiMesh *> meshes,
        const aiMatrix4x4 &rootTransform, unsigned int rootMesh,
        bool hasChild, const aiMatrix4x4 &childTransform, unsigned int childMesh,
        unsigned int numMaterials = 1) {
    aiScene *scene = new aiScene();
    scene->mNumMeshes = static_cast<unsigned int>(meshes.size());
    scene->mMeshes = new aiMesh *[scene->mNumMeshes];
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) scene->mMeshes[i] = meshes[i];

    scene->mNumMaterials = numMaterials;
    scene->mMaterials = new aiMaterial *[numMaterials];
    for (unsigned int i = 0; i < numMaterials; ++i) scene->mMaterials[i] = new aiMaterial();

    scene->mRootNode = new aiNode();
    scene->mRootNode->mName.Set("Root");
    scene->mRootNode->mTransformation = rootTransform;
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1]{rootMesh};

    if (hasChild) {
        aiNode *child = new aiNode();
        child->mName.Set("Child");
        child->mTransformation = childTransform;
        child->mParent = scene->mRootNode;
        child->mNumMeshes = 1;
        child->mMeshes = new unsigned int[1]{childMesh};
        scene->mRootNode->mNumChildren = 1;
        scene->mRootNode->mChildren = new aiNode *[1]{child};
    }
    return scene;
}

// ---------------------------------------------------------------------------
struct PtvResult {
    std::unique_ptr<aiScene> scene;
};

std::unique_ptr<aiScene> RunPtv(aiScene *scene, bool keepHierarchy, bool preserve, bool normalize = false) {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_PTV_PRESERVE_MORPHS, preserve);
    importer.SetPropertyBool(AI_CONFIG_PP_PTV_NORMALIZE, normalize);
    Assimp::PretransformVertices step;
    step.SetupProperties(&importer);
    step.KeepHierarchy(keepHierarchy);
    step.Execute(scene);
    return std::unique_ptr<aiScene>(scene);
}

// Snapshot the vertex streams of a mesh so that later mutation can be
// detected without keeping aliased pointers into it.
struct MeshSnapshot {
    std::vector<aiVector3D> positions;
    std::vector<aiVector3D> normals;
    std::vector<aiVector3D> targetPositions;
    std::vector<aiVector3D> targetNormals;
    std::vector<unsigned int> face0;
};

MeshSnapshot Snapshot(const aiMesh *mesh) {
    MeshSnapshot snap;
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) snap.positions.push_back(mesh->mVertices[i]);
    if (mesh->mNormals) {
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) snap.normals.push_back(mesh->mNormals[i]);
    }
    if (mesh->mNumAnimMeshes && mesh->mAnimMeshes[0]) {
        const aiAnimMesh *t = mesh->mAnimMeshes[0];
        if (t->mVertices) {
            for (unsigned int i = 0; i < t->mNumVertices; ++i) snap.targetPositions.push_back(t->mVertices[i]);
        }
        if (t->mNormals) {
            for (unsigned int i = 0; i < t->mNumVertices; ++i) snap.targetNormals.push_back(t->mNormals[i]);
        }
    }
    if (mesh->HasFaces()) {
        for (unsigned int i = 0; i < mesh->mFaces[0].mNumIndices; ++i) snap.face0.push_back(mesh->mFaces[0].mIndices[i]);
    }
    return snap;
}

bool SnapshotUnchanged(const aiMesh *mesh, const MeshSnapshot &snap) {
    if (mesh == nullptr || mesh->mVertices == nullptr) return false;
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        if (!NearV(mesh->mVertices[i], snap.positions[i])) return false;
    }
    for (size_t i = 0; i < snap.normals.size(); ++i) {
        if (!NearV(mesh->mNormals[i], snap.normals[i])) return false;
    }
    if (!snap.targetPositions.empty()) {
        const aiAnimMesh *t = mesh->mAnimMeshes[0];
        if (t == nullptr || t->mVertices == nullptr) return false;
        for (size_t i = 0; i < snap.targetPositions.size(); ++i) {
            if (!NearV(t->mVertices[i], snap.targetPositions[i])) return false;
        }
    }
    if (!snap.targetNormals.empty()) {
        const aiAnimMesh *t = mesh->mAnimMeshes[0];
        if (t == nullptr || t->mNormals == nullptr) return false;
        for (size_t i = 0; i < snap.targetNormals.size(); ++i) {
            if (!NearV(t->mNormals[i], snap.targetNormals[i])) return false;
        }
    }
    for (size_t i = 0; i < snap.face0.size(); ++i) {
        if (mesh->mFaces[0].mIndices[i] != snap.face0[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Standard fixture meshes.
std::vector<aiVector3D> TrianglePositions() {
    return {{1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
}
std::vector<aiVector3D> TriangleNormals() {
    return {{0.7071f, 0.7071f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
}
std::vector<aiVector3D> TriangleTargetPositions() {
    return {{2.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}};
}

aiMesh *MakeTargetMesh() {
    aiMesh *mesh = NewMesh(TrianglePositions());
    SetNormals(mesh, TriangleNormals());
    aiAnimMesh *target = AddTarget(mesh, "smile", 0.25f);
    SetTargetPositions(target, TriangleTargetPositions());
    mesh->mMethod = aiMorphingMethod_VERTEX_BLEND;
    return mesh;
}

} // namespace

// ===========================================================================
// GROUP: instances
// ===========================================================================

// The same source mesh is referenced by two nodes with different cumulative
// transforms. Both instances must bake the base and every target with their
// own matrix; the second instance must not be double-transformed.
static int case_keep_shared_mesh_transforms() {
    const aiMatrix4x4 t1 = ShearScaleTranslate();
    const aiMatrix4x4 t2 = ChildTransform();
    const aiMatrix4x4 cumulative = t1 * t2;

    aiMesh *shared = MakeTargetMesh();
    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({shared}, t1, 0, true, t2, 0), true, true);

    TASK_CHECK(scene->mNumMeshes == 2);
    TASK_CHECK(scene->mMeshes[0] != nullptr && scene->mMeshes[1] != nullptr);
    // The source keeps its targets and the copy owns its own buffers.
    TASK_CHECK(scene->mMeshes[0]->mNumAnimMeshes == 1);
    TASK_CHECK(scene->mMeshes[0]->mAnimMeshes[0] != scene->mMeshes[1]->mAnimMeshes[0]);

    const std::vector<aiVector3D> rootPos = BakedPositions(t1, TrianglePositions());
    const std::vector<aiVector3D> childPos = BakedPositions(cumulative, TrianglePositions());
    TASK_CHECK(VecArrayNear(scene->mMeshes[0]->mVertices, rootPos));
    TASK_CHECK(VecArrayNear(scene->mMeshes[1]->mVertices, childPos));

    const std::vector<aiVector3D> rootTarget = BakedPositions(t1, TriangleTargetPositions());
    const std::vector<aiVector3D> childTarget = BakedPositions(cumulative, TriangleTargetPositions());
    TASK_CHECK(VecArrayNear(scene->mMeshes[0]->mAnimMeshes[0]->mVertices, rootTarget));
    TASK_CHECK(VecArrayNear(scene->mMeshes[1]->mAnimMeshes[0]->mVertices, childTarget));

    const std::vector<aiVector3D> rootNormals = BakedNormals(t1, TriangleNormals());
    const std::vector<aiVector3D> childNormals = BakedNormals(cumulative, TriangleNormals());
    TASK_CHECK(VecArrayNear(scene->mMeshes[0]->mNormals, rootNormals));
    TASK_CHECK(VecArrayNear(scene->mMeshes[1]->mNormals, childNormals));

    const aiAnimMesh *childTargetMesh = scene->mMeshes[1]->mAnimMeshes[0];
    TASK_CHECK(childTargetMesh->mName == aiString("smile"));
    TASK_CHECK(Near(childTargetMesh->mWeight, 0.25f));
    TASK_CHECK(scene->mMeshes[1]->mMethod == aiMorphingMethod_VERTEX_BLEND);
    return 0;
}

// Collapsing the same source mesh referenced twice concatenates two segments,
// each baked with its own cumulative matrix.
static int case_collapse_shared_mesh_transforms() {
    const aiMatrix4x4 t1 = ShearScaleTranslate();
    const aiMatrix4x4 t2 = ChildTransform();
    const aiMatrix4x4 cumulative = t1 * t2;

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({MakeTargetMesh()}, t1, 0, true, t2, 0), false, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *mesh = scene->mMeshes[0];
    TASK_CHECK(mesh->mNumVertices == 6);
    TASK_CHECK(mesh->mNumAnimMeshes == 1);

    std::vector<aiVector3D> expectedBase;
    std::vector<aiVector3D> expectedTarget;
    const std::vector<aiVector3D> rootPos = BakedPositions(t1, TrianglePositions());
    const std::vector<aiVector3D> childPos = BakedPositions(cumulative, TrianglePositions());
    const std::vector<aiVector3D> rootTarget = BakedPositions(t1, TriangleTargetPositions());
    const std::vector<aiVector3D> childTarget = BakedPositions(cumulative, TriangleTargetPositions());
    expectedBase.insert(expectedBase.end(), rootPos.begin(), rootPos.end());
    expectedBase.insert(expectedBase.end(), childPos.begin(), childPos.end());
    expectedTarget.insert(expectedTarget.end(), rootTarget.begin(), rootTarget.end());
    expectedTarget.insert(expectedTarget.end(), childTarget.begin(), childTarget.end());

    TASK_CHECK(VecArrayNear(mesh->mVertices, expectedBase));
    TASK_CHECK(VecArrayNear(mesh->mAnimMeshes[0]->mVertices, expectedTarget));
    TASK_CHECK(mesh->mFaces[0].mIndices[0] == 0);
    TASK_CHECK(mesh->mFaces[1].mIndices[0] == 3);
    return 0;
}

// Complementary target channels: the target of one mesh overrides positions
// only, the target of the other overrides normals only. The merged target
// unions both channels and inherits the baked base stream for missing ones.
static int case_collapse_complementary_channels() {
    aiMesh *meshA = NewMesh(TrianglePositions());
    SetNormals(meshA, TriangleNormals());
    aiAnimMesh *targetA = AddTarget(meshA, "smile", 0.5f);
    SetTargetPositions(targetA, TriangleTargetPositions());
    meshA->mMethod = aiMorphingMethod_VERTEX_BLEND;

    aiMesh *meshB = NewMesh(TrianglePositions());
    SetNormals(meshB, TriangleNormals());
    aiAnimMesh *targetB = AddTarget(meshB, "smile", 0.5f);
    SetTargetNormals(targetB, TriangleNormals());
    meshB->mMethod = aiMorphingMethod_VERTEX_BLEND;

    const aiMatrix4x4 t1 = ShearScaleTranslate();
    const aiMatrix4x4 cumulative = t1 * ChildTransform();

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({meshA, meshB}, t1, 0, true, ChildTransform(), 1), false, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mNumVertices == 6);
    TASK_CHECK(out->mNumAnimMeshes == 1);
    aiAnimMesh *target = out->mAnimMeshes[0];

    // Positions exist on A only; B's segment inherits B's baked base.
    TASK_CHECK(target->mVertices != nullptr);
    std::vector<aiVector3D> expectedTargetPos;
    const std::vector<aiVector3D> aTarget = BakedPositions(t1, TriangleTargetPositions());
    const std::vector<aiVector3D> bBase = BakedPositions(cumulative, TrianglePositions());
    expectedTargetPos.insert(expectedTargetPos.end(), aTarget.begin(), aTarget.end());
    expectedTargetPos.insert(expectedTargetPos.end(), bBase.begin(), bBase.end());
    TASK_CHECK(VecArrayNear(target->mVertices, expectedTargetPos));

    // Normals exist on B only; A's segment inherits A's baked base.
    TASK_CHECK(target->mNormals != nullptr);
    std::vector<aiVector3D> expectedTargetNrm;
    const std::vector<aiVector3D> aBaseNrm = BakedNormals(t1, TriangleNormals());
    const std::vector<aiVector3D> bTargetNrm = BakedNormals(cumulative, TriangleNormals());
    expectedTargetNrm.insert(expectedTargetNrm.end(), aBaseNrm.begin(), aBaseNrm.end());
    expectedTargetNrm.insert(expectedTargetNrm.end(), bTargetNrm.begin(), bTargetNrm.end());
    TASK_CHECK(VecArrayNear(target->mNormals, expectedTargetNrm));

    // The target keeps the shared identity.
    TASK_CHECK(target->mName == aiString("smile"));
    TASK_CHECK(Near(target->mWeight, 0.5f));
    TASK_CHECK(out->mMethod == aiMorphingMethod_VERTEX_BLEND);
    return 0;
}

// Sparse UV/color slots (channel 0 empty, channel 1 populated) must survive
// baking and keep their values; they are not spatially transformed.
static int case_collapse_sparse_channels() {
    aiMesh *mesh = NewMesh(TrianglePositions());
    SetNormals(mesh, TriangleNormals());
    const std::vector<aiVector3D> uv1 = {{0.1f, 0.2f, 0.0f}, {0.3f, 0.4f, 0.0f}, {0.5f, 0.6f, 0.0f}};
    const std::vector<aiColor4D> col1 = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    SetChannel(mesh, 1, uv1);
    SetColorChannel(mesh, 1, col1);
    aiAnimMesh *target = AddTarget(mesh, "smile", 1.0f);
    SetTargetPositions(target, TriangleTargetPositions());
    target->mTextureCoords[1] = new aiVector3D[target->mNumVertices];
    for (unsigned int i = 0; i < target->mNumVertices; ++i) {
        target->mTextureCoords[1][i] = uv1[i];
    }

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({mesh}, ShearScaleTranslate(), 0, false, aiMatrix4x4(), 0), false, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mTextureCoords[0] == nullptr);
    TASK_CHECK(out->mTextureCoords[1] != nullptr);
    TASK_CHECK(out->mColors[0] == nullptr);
    TASK_CHECK(out->mColors[1] != nullptr);
    TASK_CHECK(VecArrayNear(out->mTextureCoords[1], uv1));
    TASK_CHECK(out->mNumUVComponents[1] == 2);
    for (unsigned int i = 0; i < out->mNumVertices; ++i) {
        TASK_CHECK(out->mColors[1][i] == col1[i]);
    }
    // Target position is baked; its UV stream stays at the original values.
    const std::vector<aiVector3D> baked = BakedPositions(ShearScaleTranslate(), TriangleTargetPositions());
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mVertices, baked));
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mTextureCoords[1], uv1));
    return 0;
}

// Same material and format but distinct target layouts produce distinct
// output meshes when collapsing. Target channels being different is not a
// layout difference and must not split buckets.
static int case_collapse_layout_partition() {
    aiMesh *smile = MakeTargetMesh(); // "smile", weight 0.25, VERTEX_BLEND
    aiMesh *frown = MakeTargetMesh(); // "smile", weight 0.25, VERTEX_BLEND
    frown->mAnimMeshes[0]->mName.Set("frown");
    aiMesh *weighted = MakeTargetMesh();
    weighted->mAnimMeshes[0]->mWeight = 0.75f;
    aiMesh *relative = MakeTargetMesh();
    relative->mMethod = aiMorphingMethod_MORPH_RELATIVE;
    aiMesh *twoTargets = MakeTargetMesh();
    aiAnimMesh *second = AddTarget(twoTargets, "blink", 0.5f);
    SetTargetPositions(second, TriangleTargetPositions());

    // smile and frown differ by name, weighted by weight, relative by method,
    // twoTargets by count. All share material 0 and the base vertex format.
    std::vector<aiMesh *> meshes{smile, frown, weighted, relative, twoTargets};
    aiScene *scene = new aiScene();
    scene->mNumMeshes = static_cast<unsigned int>(meshes.size());
    scene->mMeshes = new aiMesh *[scene->mNumMeshes];
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) scene->mMeshes[i] = meshes[i];
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1]{new aiMaterial()};
    scene->mRootNode = new aiNode();
    scene->mRootNode->mTransformation = ShearScaleTranslate();
    scene->mRootNode->mNumMeshes = static_cast<unsigned int>(meshes.size());
    scene->mRootNode->mMeshes = new unsigned int[meshes.size()];
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) scene->mRootNode->mMeshes[i] = i;

    std::unique_ptr<aiScene> result = RunPtv(scene, false, true);

    TASK_CHECK(result->mNumMeshes == 5);
    for (unsigned int i = 0; i < result->mNumMeshes; ++i) {
        TASK_CHECK(result->mMeshes[i]->mNumVertices == 3);
    }
    return 0;
}

// A mesh without targets and a mesh with targets of the same material/format
// must stay in separate output buckets.
static int case_collapse_no_target_separate() {
    aiMesh *plain = NewMesh(TrianglePositions());
    SetNormals(plain, TriangleNormals());
    aiMesh *morph = MakeTargetMesh();

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({plain, morph}, ShearScaleTranslate(), 0, true, ChildTransform(), 1), false, true);

    TASK_CHECK(scene->mNumMeshes == 2);
    unsigned int withTargets = 0;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        if (scene->mMeshes[i]->mNumAnimMeshes) ++withTargets;
    }
    TASK_CHECK(withTargets == 1);
    return 0;
}

// Name, weight, order and mMethod survive both hierarchy entry points.
static int case_keep_target_metadata_order() {
    aiMesh *mesh = NewMesh(TrianglePositions());
    SetNormals(mesh, TriangleNormals());
    aiAnimMesh *a = AddTarget(mesh, "smile", 0.25f);
    SetTargetPositions(a, TriangleTargetPositions());
    aiAnimMesh *b = AddTarget(mesh, "blink", 0.5f);
    SetTargetPositions(b, TriangleTargetPositions());
    mesh->mMethod = aiMorphingMethod_MORPH_NORMALIZED;

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({mesh}, ShearScaleTranslate(), 0, false, aiMatrix4x4(), 0), true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mNumAnimMeshes == 2);
    TASK_CHECK(out->mAnimMeshes[0]->mName == aiString("smile"));
    TASK_CHECK(out->mAnimMeshes[1]->mName == aiString("blink"));
    TASK_CHECK(Near(out->mAnimMeshes[0]->mWeight, 0.25f));
    TASK_CHECK(Near(out->mAnimMeshes[1]->mWeight, 0.5f));
    TASK_CHECK(out->mMethod == aiMorphingMethod_MORPH_NORMALIZED);
    return 0;
}

// A reflective transform flips face winding in the hierarchy-preserving
// entry point, but base and target vertex order must remain aligned so
// per-vertex morphing keeps working.
static int case_collapse_reflection_alignment() {
    aiMesh *mesh = MakeTargetMesh();
    const aiMatrix4x4 reflect = ReflectionTransform();

    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({mesh}, reflect, 0, false, aiMatrix4x4(), 0), true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mNumAnimMeshes == 1);
    // Winding inverted: originally {0,1,2}, now {2,1,0}.
    TASK_CHECK(out->mFaces[0].mIndices[0] == 2);
    TASK_CHECK(out->mFaces[0].mIndices[2] == 0);

    const std::vector<aiVector3D> base = BakedPositions(reflect, TrianglePositions());
    const std::vector<aiVector3D> target = BakedPositions(reflect, TriangleTargetPositions());
    TASK_CHECK(VecArrayNear(out->mVertices, base));
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mVertices, target));
    // Vertex 0 of both streams still describes the same vertex.
    TASK_CHECK(Near(out->mVertices[0].x, base[0].x));
    TASK_CHECK(Near(out->mAnimMeshes[0]->mVertices[0].x, target[0].x));
    return 0;
}

// Without the new option the existing behaviour is kept: the
// hierarchy-preserving mode still carries the (unbaked) targets, while the
// collapsing mode drops them.
static int case_default_off_unchanged() {
    const aiMatrix4x4 transform = ShearScaleTranslate();
    std::unique_ptr<aiScene> keep = RunPtv(
            NewScene({MakeTargetMesh()}, transform, 0, false, aiMatrix4x4(), 0), true, false);
    TASK_CHECK(keep->mNumMeshes == 1);
    // Existing keep-hierarchy result: target survives but is not baked.
    TASK_CHECK(keep->mMeshes[0]->mNumAnimMeshes == 1);
    TASK_CHECK(NearV(keep->mMeshes[0]->mAnimMeshes[0]->mVertices[0], TriangleTargetPositions()[0]));
    // ... while the base mesh is already in world space.
    TASK_CHECK(NearV(keep->mMeshes[0]->mVertices[0], BakedPositions(transform, TrianglePositions())[0]));

    std::unique_ptr<aiScene> collapse = RunPtv(
            NewScene({MakeTargetMesh()}, transform, 0, false, aiMatrix4x4(), 0), false, false);
    TASK_CHECK(collapse->mNumMeshes == 1);
    TASK_CHECK(collapse->mMeshes[0]->mNumAnimMeshes == 0);
    return 0;
}

// ===========================================================================
// GROUP: merge
// ===========================================================================

// Direct SceneCombiner::MergeMeshes with the preserve flag concatenates
// targets, owns independent arrays and leaves the inputs untouched.
static int case_direct_merge_compatible() {
    aiMesh *a = MakeTargetMesh();
    aiMesh *b = MakeTargetMesh();
    b->mName.Set("b");
    const MeshSnapshot snapA = Snapshot(a);
    const MeshSnapshot snapB = Snapshot(b);

    std::vector<aiMesh *> inputs{a, b};
    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    TASK_CHECK(out->mNumVertices == 6);
    TASK_CHECK(out->mNumFaces == 2);
    TASK_CHECK(out->mNumAnimMeshes == 1);
    TASK_CHECK(out->mMethod == aiMorphingMethod_VERTEX_BLEND);
    TASK_CHECK(out->mAnimMeshes[0]->mName == aiString("smile"));
    TASK_CHECK(Near(out->mAnimMeshes[0]->mWeight, 0.25f));

    // Output buffers are independent from the inputs.
    TASK_CHECK(out->mVertices != a->mVertices);
    TASK_CHECK(out->mAnimMeshes[0]->mVertices != a->mAnimMeshes[0]->mVertices);
    TASK_CHECK(out->mAnimMeshes[0]->mVertices != b->mAnimMeshes[0]->mVertices);
    TASK_CHECK(out->mFaces[0].mIndices != a->mFaces[0].mIndices);

    // Face indices are offset and keep alignment with target vertices.
    TASK_CHECK(out->mFaces[1].mIndices[0] == 3);
    std::vector<aiVector3D> expectedTarget;
    for (unsigned int i = 0; i < 3; ++i) expectedTarget.push_back(TriangleTargetPositions()[i]);
    for (unsigned int i = 0; i < 3; ++i) expectedTarget.push_back(TriangleTargetPositions()[i]);
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mVertices, expectedTarget));
    TASK_CHECK(out->mAnimMeshes[0]->mVertices[3].x == 2.0f);
    TASK_CHECK(out->mAnimMeshes[0]->mVertices[4].x == 3.0f);

    // The inputs were not modified.
    TASK_CHECK(SnapshotUnchanged(a, snapA));
    TASK_CHECK(SnapshotUnchanged(b, snapB));

    // Destroying the output must not affect the (still alive) inputs.
    out.reset();
    TASK_CHECK(SnapshotUnchanged(a, snapA));
    TASK_CHECK(SnapshotUnchanged(b, snapB));

    delete a;
    delete b;
    return 0;
}

// Complementary target channels union in the direct merge too, inheriting
// from the output's concatenated base streams.
static int case_direct_merge_complementary_channels() {
    aiMesh *a = NewMesh(TrianglePositions());
    SetNormals(a, TriangleNormals());
    aiAnimMesh *ta = AddTarget(a, "smile", 0.5f);
    SetTargetPositions(ta, TriangleTargetPositions());
    a->mMethod = aiMorphingMethod_VERTEX_BLEND;

    aiMesh *b = NewMesh(TrianglePositions());
    SetNormals(b, TriangleNormals());
    aiAnimMesh *tb = AddTarget(b, "smile", 0.5f);
    SetTargetNormals(tb, TriangleNormals());
    b->mMethod = aiMorphingMethod_VERTEX_BLEND;

    std::vector<aiMesh *> inputs{a, b};
    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    aiAnimMesh *target = out->mAnimMeshes[0];
    TASK_CHECK(target->mVertices != nullptr);
    TASK_CHECK(target->mNormals != nullptr);

    // Segment A: explicit target positions, inherited base normals.
    TASK_CHECK(NearV(target->mVertices[0], TriangleTargetPositions()[0]));
    TASK_CHECK(NearV(target->mNormals[0], TriangleNormals()[0]));
    // Segment B: inherited base positions, explicit target normals.
    TASK_CHECK(NearV(target->mVertices[3], TrianglePositions()[0]));
    TASK_CHECK(NearV(target->mNormals[3], TriangleNormals()[0]));
    // A channel nobody overrides stays empty.
    TASK_CHECK(target->mTangents == nullptr);
    TASK_CHECK(target->mBitangents == nullptr);

    delete a;
    delete b;
    return 0;
}

// Sparse UV/color slots survive the direct merge.
static int case_direct_merge_sparse_slots() {
    aiMesh *a = NewMesh(TrianglePositions());
    aiMesh *b = NewMesh(TrianglePositions());
    const std::vector<aiVector3D> uv1 = {{0.1f, 0.2f, 0.0f}, {0.3f, 0.4f, 0.0f}, {0.5f, 0.6f, 0.0f}};
    const std::vector<aiColor4D> col1 = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    SetChannel(a, 1, uv1);
    SetColorChannel(a, 1, col1);
    aiAnimMesh *ta = AddTarget(a, "smile", 0.5f);
    SetTargetPositions(ta, TriangleTargetPositions());
    aiAnimMesh *tb = AddTarget(b, "smile", 0.5f);
    SetTargetPositions(tb, TriangleTargetPositions());

    std::vector<aiMesh *> inputs{a, b};
    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    TASK_CHECK(out->mTextureCoords[0] == nullptr);
    TASK_CHECK(out->mTextureCoords[1] != nullptr);
    TASK_CHECK(out->mColors[0] == nullptr);
    TASK_CHECK(out->mColors[1] != nullptr);
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(NearV(out->mTextureCoords[1][i], uv1[i]));
        TASK_CHECK(out->mColors[1][i] == col1[i]);
    }

    delete a;
    delete b;
    return 0;
}

// Merging the same inputs twice must produce two independent outputs and
// never consume or mutate the inputs.
static int case_direct_merge_lifecycle() {
    aiMesh *a = MakeTargetMesh();
    aiMesh *b = MakeTargetMesh();
    const MeshSnapshot snapA = Snapshot(a);
    const MeshSnapshot snapB = Snapshot(b);
    std::vector<aiMesh *> inputs{a, b};

    std::unique_ptr<aiMesh> first;
    std::unique_ptr<aiMesh> second;
    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    first.reset(raw);
    TASK_CHECK(first != nullptr);

    raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    second.reset(raw);
    TASK_CHECK(second != nullptr);

    TASK_CHECK(first->mVertices != second->mVertices);
    TASK_CHECK(first->mAnimMeshes[0] != second->mAnimMeshes[0]);
    TASK_CHECK(SnapshotUnchanged(a, snapA));
    TASK_CHECK(SnapshotUnchanged(b, snapB));

    first.reset();
    second.reset();
    TASK_CHECK(SnapshotUnchanged(a, snapA));
    TASK_CHECK(SnapshotUnchanged(b, snapB));

    delete a;
    delete b;
    return 0;
}

// Incompatible ordered layouts throw std::invalid_argument.
static int case_direct_merge_incompatible_throws() {
    std::vector<aiMesh *> inputs;

    // Different target name.
    aiMesh *a = MakeTargetMesh();
    aiMesh *b = MakeTargetMesh();
    b->mAnimMeshes[0]->mName.Set("frown");
    inputs.assign({a, b});
    bool threw = false;
    try {
        aiMesh *raw = nullptr;
        Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                inputs.begin(), inputs.end());
        delete raw;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    delete a;
    delete b;

    // Different weight.
    a = MakeTargetMesh();
    b = MakeTargetMesh();
    b->mAnimMeshes[0]->mWeight = 0.75f;
    inputs.assign({a, b});
    threw = false;
    try {
        aiMesh *raw = nullptr;
        Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                inputs.begin(), inputs.end());
        delete raw;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    delete a;
    delete b;

    // Different morphing method.
    a = MakeTargetMesh();
    b = MakeTargetMesh();
    b->mMethod = aiMorphingMethod_MORPH_RELATIVE;
    inputs.assign({a, b});
    threw = false;
    try {
        aiMesh *raw = nullptr;
        Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                inputs.begin(), inputs.end());
        delete raw;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    delete a;
    delete b;

    // One mesh without targets, one with targets.
    a = NewMesh(TrianglePositions());
    SetNormals(a, TriangleNormals());
    b = MakeTargetMesh();
    inputs.assign({a, b});
    threw = false;
    try {
        aiMesh *raw = nullptr;
        Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
                inputs.begin(), inputs.end());
        delete raw;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);
    delete a;
    delete b;
    return 0;
}

// The empty range still yields nullptr.
static int case_direct_merge_empty_range() {
    std::vector<aiMesh *> inputs;
    aiMesh *raw = reinterpret_cast<aiMesh *>(0x1); // must be overwritten
    Assimp::SceneCombiner::MergeMeshes(&raw, AI_INT_MERGE_MESHES_PRESERVE_MORPHS,
            inputs.begin(), inputs.end());
    TASK_CHECK(raw == nullptr);

    raw = reinterpret_cast<aiMesh *>(0x1);
    Assimp::SceneCombiner::MergeMeshes(&raw, 0, inputs.begin(), inputs.end());
    TASK_CHECK(raw == nullptr);
    return 0;
}

// flags == 0 keeps the historical behaviour: targets are dropped and the
// merge consumes its inputs.
static int case_direct_merge_flags_zero() {
    aiMesh *a = NewMesh(TrianglePositions());
    aiMesh *b = NewMesh(TrianglePositions());
    std::vector<aiMesh *> inputs{a, b};
    aiMesh *raw = nullptr;
    Assimp::SceneCombiner::MergeMeshes(&raw, 0, inputs.begin(), inputs.end());
    std::unique_ptr<aiMesh> out(raw);

    TASK_CHECK(out != nullptr);
    TASK_CHECK(out->mNumVertices == 6);
    TASK_CHECK(out->mNumFaces == 2);
    TASK_CHECK(out->mNumAnimMeshes == 0);
    // The inputs are consumed by the historical implementation; nothing to
    // delete here.
    return 0;
}

// ===========================================================================
// GROUP: normalize
// ===========================================================================

static const std::vector<aiVector3D> NormalizeBase = {
        {-2.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
static const std::vector<aiVector3D> NormalizeTarget = {
        {-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}};

aiMesh *MakeNormalizableMesh() {
    aiMesh *mesh = NewMesh(NormalizeBase);
    SetNormals(mesh, TriangleNormals());
    aiAnimMesh *target = AddTarget(mesh, "smile", 0.5f);
    SetTargetPositions(target, NormalizeTarget);
    mesh->mMethod = aiMorphingMethod_VERTEX_BLEND;
    return mesh;
}

// Expected mapping from the baked base bounding box: centre + half of the
// dominant extent. The same map is applied to all target positions.
struct NormalizeMap {
    aiVector3D center;
    float divisor;
};

NormalizeMap ExpectedNormalizeMap(const std::vector<aiVector3D> &bakedBase) {
    aiVector3D min = bakedBase[0], max = bakedBase[0];
    for (size_t i = 1; i < bakedBase.size(); ++i) {
        min.x = std::min(min.x, bakedBase[i].x);
        min.y = std::min(min.y, bakedBase[i].y);
        min.z = std::min(min.z, bakedBase[i].z);
        max.x = std::max(max.x, bakedBase[i].x);
        max.y = std::max(max.y, bakedBase[i].y);
        max.z = std::max(max.z, bakedBase[i].z);
    }
    aiVector3D extent = max - min;
    const float dominant = std::max(extent.x, std::max(extent.y, extent.z));
    NormalizeMap map;
    map.center = min + extent * 0.5f;
    map.divisor = dominant * 0.5f;
    return map;
}

std::vector<aiVector3D> ApplyNormalizeMap(const NormalizeMap &map, const std::vector<aiVector3D> &values) {
    std::vector<aiVector3D> out;
    for (size_t i = 0; i < values.size(); ++i) {
        out.push_back((values[i] - map.center) / map.divisor);
    }
    return out;
}

// Collapse mode: normalize uses only the baked base bounding box, then maps
// all target positions with the very same transform.
static int case_collapse_normalize() {
    const aiMatrix4x4 transform = ShearScaleTranslate();
    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({MakeNormalizableMesh()}, transform, 0, false, aiMatrix4x4(), 0),
            false, true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mNumAnimMeshes == 1);

    const std::vector<aiVector3D> bakedBase = BakedPositions(transform, NormalizeBase);
    const std::vector<aiVector3D> bakedTarget = BakedPositions(transform, NormalizeTarget);
    const NormalizeMap map = ExpectedNormalizeMap(bakedBase);

    TASK_CHECK(VecArrayNear(out->mVertices, ApplyNormalizeMap(map, bakedBase)));
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mVertices, ApplyNormalizeMap(map, bakedTarget)));
    // The base is inside [-1, 1] on every axis; the target deliberately goes
    // beyond it because it is not used to size the mapping.
    TASK_CHECK(out->mVertices[0].x >= -1.0001f && out->mVertices[0].x <= 1.0001f);
    bool targetExceeds = false;
    for (unsigned int i = 0; i < out->mAnimMeshes[0]->mNumVertices; ++i) {
        const aiVector3D &v = out->mAnimMeshes[0]->mVertices[i];
        if (std::fabs(v.x) > 1.0001f || std::fabs(v.y) > 1.0001f || std::fabs(v.z) > 1.0001f) {
            targetExceeds = true;
        }
    }
    TASK_CHECK(targetExceeds);
    return 0;
}

// Keep-hierarchy entry point applies the same normalize mapping to targets.
static int case_keep_normalize() {
    const aiMatrix4x4 transform = ShearScaleTranslate();
    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({MakeNormalizableMesh()}, transform, 0, false, aiMatrix4x4(), 0),
            true, true, true);

    TASK_CHECK(scene->mNumMeshes == 1);
    aiMesh *out = scene->mMeshes[0];
    TASK_CHECK(out->mNumAnimMeshes == 1);

    const std::vector<aiVector3D> bakedBase = BakedPositions(transform, NormalizeBase);
    const std::vector<aiVector3D> bakedTarget = BakedPositions(transform, NormalizeTarget);
    const NormalizeMap map = ExpectedNormalizeMap(bakedBase);

    TASK_CHECK(VecArrayNear(out->mVertices, ApplyNormalizeMap(map, bakedBase)));
    TASK_CHECK(VecArrayNear(out->mAnimMeshes[0]->mVertices, ApplyNormalizeMap(map, bakedTarget)));
    return 0;
}

// Normalization is a single uniform scale for everything; check the ratio
// between base and target is preserved (target is not scaled per-target).
static int case_normalize_uniform_mapping() {
    const aiMatrix4x4 transform = ShearScaleTranslate();
    std::unique_ptr<aiScene> scene = RunPtv(
            NewScene({MakeNormalizableMesh()}, transform, 0, false, aiMatrix4x4(), 0),
            false, true, true);

    aiMesh *out = scene->mMeshes[0];
    aiAnimMesh *target = out->mAnimMeshes[0];
    const std::vector<aiVector3D> bakedBase = BakedPositions(transform, NormalizeBase);
    const std::vector<aiVector3D> bakedTarget = BakedPositions(transform, NormalizeTarget);
    const NormalizeMap map = ExpectedNormalizeMap(bakedBase);

    // Explicit numeric reference: baked base spans x in [0, 40] (scale 2 plus
    // translation 10 => [-2,2] -> [6,14], [2,2] -> [14,14]; y span is
    // [20, 26] after the 3x scale and 20 translation). Dominant extent is
    // therefore 8, divisor 4, center (10, 23, 30).
    const float divisor = map.divisor;
    TASK_CHECK(Near(divisor, 4.0f));
    TASK_CHECK(Near(map.center.x, 10.0f));
    TASK_CHECK(Near(map.center.y, 23.0f));
    TASK_CHECK(Near(map.center.z, 30.0f));

    // The target must be mapped with the base-derived divisor, not with its
    // own bounding box. Target x spans [2, 18] after baking -> normalized
    // [-2, 2] with divisor 4, exceeding the base range.
    for (unsigned int i = 0; i < target->mNumVertices; ++i) {
        const std::vector<aiVector3D> normBase = ApplyNormalizeMap(map, bakedBase);
        const std::vector<aiVector3D> normTarget = ApplyNormalizeMap(map, bakedTarget);
        TASK_CHECK(NearV(out->mVertices[i], normBase[i]));
        TASK_CHECK(NearV(target->mVertices[i], normTarget[i]));
    }
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

extern const struct task_case task_cases[] = {
    {"instances", "keep_shared_mesh_transforms", case_keep_shared_mesh_transforms},
    {"instances", "collapse_shared_mesh_transforms", case_collapse_shared_mesh_transforms},
    {"instances", "collapse_complementary_channels", case_collapse_complementary_channels},
    {"instances", "collapse_sparse_channels", case_collapse_sparse_channels},
    {"instances", "collapse_layout_partition", case_collapse_layout_partition},
    {"instances", "collapse_no_target_separate", case_collapse_no_target_separate},
    {"instances", "keep_target_metadata_order", case_keep_target_metadata_order},
    {"instances", "collapse_reflection_alignment", case_collapse_reflection_alignment},
    {"instances", "default_off_unchanged", case_default_off_unchanged},
    {"merge", "direct_merge_compatible", case_direct_merge_compatible},
    {"merge", "direct_merge_complementary_channels", case_direct_merge_complementary_channels},
    {"merge", "direct_merge_sparse_slots", case_direct_merge_sparse_slots},
    {"merge", "direct_merge_lifecycle", case_direct_merge_lifecycle},
    {"merge", "direct_merge_incompatible_throws", case_direct_merge_incompatible_throws},
    {"merge", "direct_merge_empty_range", case_direct_merge_empty_range},
    {"merge", "direct_merge_flags_zero", case_direct_merge_flags_zero},
    {"normalize", "collapse_normalize", case_collapse_normalize},
    {"normalize", "keep_normalize", case_keep_normalize},
    {"normalize", "uniform_mapping", case_normalize_uniform_mapping},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
