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

/// @file  PretransformVertices.cpp
/// @brief Implementation of the "PretransformVertices" post processing step

#include "PretransformVertices.h"
#include "ConvertToLHProcess.h"
#include "MorphTargetBake.h"
#include "ProcessHelper.h"
#include <assimp/Exceptional.h>
#include <assimp/SceneCombiner.h>

using namespace Assimp;

// some array offsets
#define AI_PTVS_VERTEX 0x0
#define AI_PTVS_FACE 0x1

namespace {

// Get a bitwise combination identifying the vertex format of a mesh
static unsigned int GetMeshVFormat(aiMesh *pcMesh)  {
	// the vertex format is stored in aiMesh::mBones for later retrieval.
	// there isn't a good reason to compute it a few hundred times
	// from scratch. The pointer is unused as animations are lost
	// during PretransformVertices.
	if (pcMesh->mBones)
		return (unsigned int)(uint64_t)pcMesh->mBones;

	const unsigned int iRet = GetMeshVFormatUnique(pcMesh);

	// store the value for later use
	pcMesh->mBones = (aiBone **)(uint64_t)iRet;
	return iRet;
}

// Get a list of all vertex formats that occur for a given material index
// The output list contains duplicate elements
static void GetVFormatList(const aiScene *pcScene, unsigned int iMat, std::list<unsigned int> &aiOut)  {
	for (unsigned int i = 0; i < pcScene->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[i];
		if (iMat == pcMesh->mMaterialIndex) {
			aiOut.push_back(GetMeshVFormat(pcMesh));
		}
	}
}

// Sparse-aware vertex format for morph-preserving collapses: every
// non-null UV/color slot contributes, even when an earlier slot is empty.
// The bit layout matches GetMeshVFormatUnique().
static unsigned int GetMeshVFormatSparse(const aiMesh *mesh) {
	unsigned int fmt = 1;
	if (mesh->mNormals) {
		fmt |= 0x2;
	}
	if (mesh->mTangents && mesh->mBitangents) {
		fmt |= 0x4;
	}
	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++p) {
		if (mesh->mTextureCoords[p]) {
			fmt |= (0x100u << p);
			if (mesh->mNumUVComponents[p] == 3) {
				fmt |= (0x10000u << p);
			}
		}
	}
	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_COLOR_SETS; ++p) {
		if (mesh->mColors[p]) {
			fmt |= (0x1000000u << p);
		}
	}
	return fmt;
}

// One output bucket of the morph-preserving collapse: meshes with the
// same material, sparse vertex format and ordered morph target layout.
struct MorphBakeGroup {
	unsigned int material;
	unsigned int vformat;
	const aiMesh *layout;
	unsigned int numVertices;
	unsigned int numFaces;
	std::vector<MorphTargetBake::Segment> segments;

	MorphBakeGroup(unsigned int mat, unsigned int fmt, const aiMesh *first) :
			material(mat), vformat(fmt), layout(first), numVertices(0), numFaces(0) {}
};

// First traversal: assign every node/mesh reference to a bake group.
static void FindMorphBakeGroups(const aiScene *scene, const aiNode *node,
		std::vector<MorphBakeGroup> &groups) {
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		const aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
		const unsigned int fmt = GetMeshVFormatSparse(mesh);

		MorphBakeGroup *group = nullptr;
		for (MorphBakeGroup &candidate : groups) {
			if (candidate.material == mesh->mMaterialIndex && candidate.vformat == fmt &&
					MorphTargetBake::SameMorphLayout(*mesh, *candidate.layout)) {
				group = &candidate;
				break;
			}
		}
		if (!group) {
			groups.emplace_back(mesh->mMaterialIndex, fmt, mesh);
			group = &groups.back();
		}
		group->numVertices += mesh->mNumVertices;
		group->numFaces += mesh->mNumFaces;
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		FindMorphBakeGroups(scene, node->mChildren[i], groups);
	}
}

// Allocate the base streams of a collapsed output mesh for a sparse
// vertex format.
static aiMesh *AllocateBakedMesh(const MorphBakeGroup &group) {
	aiMesh *out = new aiMesh();
	const unsigned int fmt = group.vformat;
	out->mMaterialIndex = group.material;
	out->mNumVertices = group.numVertices;
	out->mNumFaces = group.numFaces;
	out->mFaces = new aiFace[group.numFaces];
	out->mVertices = new aiVector3D[group.numVertices];
	if (fmt & 0x2) {
		out->mNormals = new aiVector3D[group.numVertices];
	}
	if (fmt & 0x4) {
		out->mTangents = new aiVector3D[group.numVertices];
		out->mBitangents = new aiVector3D[group.numVertices];
	}
	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++p) {
		if (fmt & (0x100u << p)) {
			out->mTextureCoords[p] = new aiVector3D[group.numVertices];
			out->mNumUVComponents[p] = (fmt & (0x10000u << p)) ? 3 : 2;
		}
	}
	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_COLOR_SETS; ++p) {
		if (fmt & (0x1000000u << p)) {
			out->mColors[p] = new aiColor4D[group.numVertices];
		}
	}
	return out;
}

// Second traversal: fill the base streams and faces of a bake group and
// record the segments needed to stitch its morph targets.
static void CollectMorphBakeGroup(const aiScene *scene, const aiNode *node,
		MorphBakeGroup &group, aiMesh *out, unsigned int *num_refs,
		unsigned int &curVertex, unsigned int &curFace) {
	const bool identity = node->mTransformation.IsIdentity();
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		const unsigned int meshIndex = node->mMeshes[i];
		aiMesh *mesh = scene->mMeshes[meshIndex];
		if (mesh->mMaterialIndex != group.material ||
				GetMeshVFormatSparse(mesh) != group.vformat ||
				!MorphTargetBake::SameMorphLayout(*mesh, *group.layout)) {
			continue;
		}

		unsigned int &num_ref = num_refs[meshIndex];
		ai_assert(0 != num_ref);
		--num_ref;
		if (num_ref == 0) {
			out->mName = mesh->mName;
		}

		const unsigned int n = mesh->mNumVertices;
		const unsigned int vBase = curVertex;
		if (identity) {
			::memcpy(out->mVertices + vBase, mesh->mVertices, n * sizeof(aiVector3D));
			if (mesh->mNormals) {
				::memcpy(out->mNormals + vBase, mesh->mNormals, n * sizeof(aiVector3D));
			}
			if (mesh->mTangents) {
				::memcpy(out->mTangents + vBase, mesh->mTangents, n * sizeof(aiVector3D));
				::memcpy(out->mBitangents + vBase, mesh->mBitangents, n * sizeof(aiVector3D));
			}
		} else {
			for (unsigned int v = 0; v < n; ++v) {
				out->mVertices[vBase + v] = node->mTransformation * mesh->mVertices[v];
			}
			const aiMatrix3x3 normal = aiMatrix3x3(node->mTransformation).Inverse().Transpose();
			if (mesh->mNormals) {
				for (unsigned int v = 0; v < n; ++v) {
					out->mNormals[vBase + v] = (normal * mesh->mNormals[v]).Normalize();
				}
			}
			if (mesh->mTangents) {
				for (unsigned int v = 0; v < n; ++v) {
					out->mTangents[vBase + v] = (normal * mesh->mTangents[v]).Normalize();
					out->mBitangents[vBase + v] = (normal * mesh->mBitangents[v]).Normalize();
				}
			}
		}
		for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++p) {
			if (mesh->mTextureCoords[p]) {
				::memcpy(out->mTextureCoords[p] + vBase, mesh->mTextureCoords[p],
						n * sizeof(aiVector3D));
			}
		}
		for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_COLOR_SETS; ++p) {
			if (mesh->mColors[p]) {
				::memcpy(out->mColors[p] + vBase, mesh->mColors[p], n * sizeof(aiColor4D));
			}
		}

		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			aiFace &fSrc = mesh->mFaces[f];
			aiFace &fDst = out->mFaces[curFace + f];
			const unsigned int numIdx = fSrc.mNumIndices;
			fDst.mNumIndices = numIdx;
			unsigned int *indices;
			if (!num_ref) {
				indices = fDst.mIndices = fSrc.mIndices;
			} else {
				indices = fDst.mIndices = new unsigned int[numIdx];
				::memcpy(indices, fSrc.mIndices, numIdx * sizeof(unsigned int));
			}
			for (unsigned int q = 0; q < numIdx; ++q) {
				indices[q] += vBase;
			}
			switch (numIdx) {
				case 0x1:
					out->mPrimitiveTypes |= aiPrimitiveType_POINT;
					break;
				case 0x2:
					out->mPrimitiveTypes |= aiPrimitiveType_LINE;
					break;
				case 0x3:
					out->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;
					break;
				default:
					out->mPrimitiveTypes |= aiPrimitiveType_POLYGON;
					break;
			}
		}

		MorphTargetBake::Segment segment;
		segment.source = mesh;
		segment.transform = identity ? nullptr : &node->mTransformation;
		segment.bakedBase = out;
		segment.baseOffset = vBase;
		segment.outOffset = vBase;
		segment.vertexCount = n;
		group.segments.push_back(segment);

		curVertex += n;
		curFace += mesh->mNumFaces;
	}

	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		CollectMorphBakeGroup(scene, node->mChildren[i], group, out, num_refs, curVertex, curFace);
	}
}

}
// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
PretransformVertices::PretransformVertices() :
		mConfigKeepHierarchy(false),
		mConfigNormalize(false),
		mConfigTransform(false),
		mConfigTransformation(),
		mConfigPointCloud(false),
		mConfigPreserveMorphs(false) {}

// ------------------------------------------------------------------------------------------------
// Returns whether the processing step is present in the given flag field.
bool PretransformVertices::IsActive(unsigned int pFlags) const {
	return (pFlags & aiProcess_PreTransformVertices) != 0;
}

// ------------------------------------------------------------------------------------------------
// Setup import configuration
void PretransformVertices::SetupProperties(const Importer *pImp) {
	// Get the current value of AI_CONFIG_PP_PTV_KEEP_HIERARCHY, AI_CONFIG_PP_PTV_NORMALIZE,
	// AI_CONFIG_PP_PTV_PRESERVE_MORPHS, AI_CONFIG_PP_PTV_ADD_ROOT_TRANSFORMATION and
	// AI_CONFIG_PP_PTV_ROOT_TRANSFORMATION
	mConfigKeepHierarchy = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_KEEP_HIERARCHY, 0));
	mConfigNormalize = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_NORMALIZE, 0));
	mConfigPreserveMorphs = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_PRESERVE_MORPHS, 0));
	mConfigTransform = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_ADD_ROOT_TRANSFORMATION, 0));

	mConfigTransformation = pImp->GetPropertyMatrix(AI_CONFIG_PP_PTV_ROOT_TRANSFORMATION, aiMatrix4x4());

	mConfigPointCloud = pImp->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);
}

// ------------------------------------------------------------------------------------------------
// Count the number of nodes
unsigned int PretransformVertices::CountNodes(const aiNode *pcNode) const {
	unsigned int iRet = 1;
	for (unsigned int i = 0; i < pcNode->mNumChildren; ++i) {
		iRet += CountNodes(pcNode->mChildren[i]);
	}
	return iRet;
}

// ------------------------------------------------------------------------------------------------
// Count the number of vertices in the whole scene and a given material index
void PretransformVertices::CountVerticesAndFaces(const aiScene *pcScene, const aiNode *pcNode, unsigned int iMat,
		unsigned int iVFormat, unsigned int *piFaces, unsigned int *piVertices) const {
	for (unsigned int i = 0; i < pcNode->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[pcNode->mMeshes[i]];
		if (iMat == pcMesh->mMaterialIndex && iVFormat == GetMeshVFormat(pcMesh)) {
			*piVertices += pcMesh->mNumVertices;
			*piFaces += pcMesh->mNumFaces;
		}
	}
	for (unsigned int i = 0; i < pcNode->mNumChildren; ++i) {
		CountVerticesAndFaces(pcScene, pcNode->mChildren[i], iMat, iVFormat, piFaces, piVertices);
	}
}

// ------------------------------------------------------------------------------------------------
// Collect vertex/face data
void PretransformVertices::CollectData(const aiScene *pcScene, const aiNode *pcNode, unsigned int iMat,
		unsigned int iVFormat, aiMesh *pcMeshOut,
		unsigned int aiCurrent[2], unsigned int *num_refs) const {
	// No need to multiply if there's no transformation
	const bool identity = pcNode->mTransformation.IsIdentity();
	for (unsigned int i = 0; i < pcNode->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[pcNode->mMeshes[i]];
		if (iMat == pcMesh->mMaterialIndex && iVFormat == GetMeshVFormat(pcMesh)) {
			// Decrement mesh reference counter
			unsigned int &num_ref = num_refs[pcNode->mMeshes[i]];
			ai_assert(0 != num_ref);
			--num_ref;
			// Save the name of the last mesh
			if (num_ref == 0) {
				pcMeshOut->mName = pcMesh->mName;
			}

			if (identity) {
				// copy positions without modifying them
				::memcpy(pcMeshOut->mVertices + aiCurrent[AI_PTVS_VERTEX],
						pcMesh->mVertices,
						pcMesh->mNumVertices * sizeof(aiVector3D));

				if (iVFormat & 0x2) {
					// copy normals without modifying them
					::memcpy(pcMeshOut->mNormals + aiCurrent[AI_PTVS_VERTEX],
							pcMesh->mNormals,
							pcMesh->mNumVertices * sizeof(aiVector3D));
				}
				if (iVFormat & 0x4) {
					// copy tangents without modifying them
					::memcpy(pcMeshOut->mTangents + aiCurrent[AI_PTVS_VERTEX],
							pcMesh->mTangents,
							pcMesh->mNumVertices * sizeof(aiVector3D));
					// copy bitangents without modifying them
					::memcpy(pcMeshOut->mBitangents + aiCurrent[AI_PTVS_VERTEX],
							pcMesh->mBitangents,
							pcMesh->mNumVertices * sizeof(aiVector3D));
				}
			} else {
				// copy positions, transform them to worldspace
				for (unsigned int n = 0; n < pcMesh->mNumVertices; ++n) {
					pcMeshOut->mVertices[aiCurrent[AI_PTVS_VERTEX] + n] = pcNode->mTransformation * pcMesh->mVertices[n];
				}
				aiMatrix4x4 mWorldIT = pcNode->mTransformation;
				mWorldIT.Inverse().Transpose();

				// TODO: implement Inverse() for aiMatrix3x3
				aiMatrix3x3 m = aiMatrix3x3(mWorldIT);

				if (iVFormat & 0x2) {
					// copy normals, transform them to worldspace
					for (unsigned int n = 0; n < pcMesh->mNumVertices; ++n) {
						pcMeshOut->mNormals[aiCurrent[AI_PTVS_VERTEX] + n] =
								(m * pcMesh->mNormals[n]).Normalize();
					}
				}
				if (iVFormat & 0x4) {
					// copy tangents and bitangents, transform them to worldspace
					for (unsigned int n = 0; n < pcMesh->mNumVertices; ++n) {
						pcMeshOut->mTangents[aiCurrent[AI_PTVS_VERTEX] + n] = (m * pcMesh->mTangents[n]).Normalize();
						pcMeshOut->mBitangents[aiCurrent[AI_PTVS_VERTEX] + n] = (m * pcMesh->mBitangents[n]).Normalize();
					}
				}
			}
			unsigned int p = 0;
			while (iVFormat & (0x100 << p)) {
				// copy texture coordinates
				memcpy(pcMeshOut->mTextureCoords[p] + aiCurrent[AI_PTVS_VERTEX],
						pcMesh->mTextureCoords[p],
						pcMesh->mNumVertices * sizeof(aiVector3D));
				++p;
			}
			p = 0;
			while (iVFormat & (0x1000000 << p)) {
				// copy vertex colors
				memcpy(pcMeshOut->mColors[p] + aiCurrent[AI_PTVS_VERTEX],
						pcMesh->mColors[p],
						pcMesh->mNumVertices * sizeof(aiColor4D));
				++p;
			}
			// now we need to copy all faces. since we will delete the source mesh afterwards,
			// we don't need to reallocate the array of indices except if this mesh is
			// referenced multiple times.
			for (unsigned int planck = 0; planck < pcMesh->mNumFaces; ++planck) {
				aiFace &f_src = pcMesh->mFaces[planck];
				aiFace &f_dst = pcMeshOut->mFaces[aiCurrent[AI_PTVS_FACE] + planck];

				const unsigned int num_idx = f_src.mNumIndices;

				f_dst.mNumIndices = num_idx;

				unsigned int *pi;
				if (!num_ref) { /* if last time the mesh is referenced -> no reallocation */
					pi = f_dst.mIndices = f_src.mIndices;

					// offset all vertex indices
					for (unsigned int hahn = 0; hahn < num_idx; ++hahn) {
						pi[hahn] += aiCurrent[AI_PTVS_VERTEX];
					}
				} else {
					pi = f_dst.mIndices = new unsigned int[num_idx];

					// copy and offset all vertex indices
					for (unsigned int hahn = 0; hahn < num_idx; ++hahn) {
						pi[hahn] = f_src.mIndices[hahn] + aiCurrent[AI_PTVS_VERTEX];
					}
				}

				// Update the mPrimitiveTypes member of the mesh
				switch (pcMesh->mFaces[planck].mNumIndices) {
					case 0x1:
						pcMeshOut->mPrimitiveTypes |= aiPrimitiveType_POINT;
						break;
					case 0x2:
						pcMeshOut->mPrimitiveTypes |= aiPrimitiveType_LINE;
						break;
					case 0x3:
						pcMeshOut->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;
						break;
					default:
						pcMeshOut->mPrimitiveTypes |= aiPrimitiveType_POLYGON;
						break;
				};
			}
			aiCurrent[AI_PTVS_VERTEX] += pcMesh->mNumVertices;
			aiCurrent[AI_PTVS_FACE] += pcMesh->mNumFaces;
		}
	}

	// append all children of us
	for (unsigned int i = 0; i < pcNode->mNumChildren; ++i) {
		CollectData(pcScene, pcNode->mChildren[i], iMat,
				iVFormat, pcMeshOut, aiCurrent, num_refs);
	}
}

// ------------------------------------------------------------------------------------------------
// Compute the absolute transformation matrices of each node
void PretransformVertices::ComputeAbsoluteTransform(aiNode *pcNode) {
	if (pcNode->mParent) {
		pcNode->mTransformation = pcNode->mParent->mTransformation * pcNode->mTransformation;
	}

	for (unsigned int i = 0; i < pcNode->mNumChildren; ++i) {
		ComputeAbsoluteTransform(pcNode->mChildren[i]);
	}
}

// ------------------------------------------------------------------------------------------------
// Apply the node transformation to a mesh
void PretransformVertices::ApplyTransform(aiMesh *mesh, const aiMatrix4x4 &mat) const {
	// Check whether we need to transform the coordinates at all
	if (mat.IsIdentity()) {
		return;
	}

	// Check for odd negative scale (mirror)
	if (mesh->HasFaces() && mat.Determinant() < 0) {
		// Reverse the mesh face winding order
		FlipWindingOrderProcess::ProcessMesh(mesh);
	}

	// Update positions
	if (mesh->HasPositions()) {
		for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
			mesh->mVertices[i] = mat * mesh->mVertices[i];
		}
	}

	// Update normals and tangents
	if (mesh->HasNormals() || mesh->HasTangentsAndBitangents()) {
		const aiMatrix3x3 m = aiMatrix3x3(mat).Inverse().Transpose();

		if (mesh->HasNormals()) {
			for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
                mesh->mNormals[i] = (m * mesh->mNormals[i]).Normalize();
            }
		}

		if (mesh->HasTangentsAndBitangents()) {
			for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
				mesh->mTangents[i] = (m * mesh->mTangents[i]).Normalize();
				mesh->mBitangents[i] = (m * mesh->mBitangents[i]).Normalize();
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Simple routine to build meshes in worldspace, no further optimization
void PretransformVertices::BuildWCSMeshes(std::vector<aiMesh *> &out, aiMesh **in,
		unsigned int numIn, aiNode *node) const {
	// NOTE:
	//  aiMesh::mNumBones store original source mesh, or UINT_MAX if not a copy
	//  aiMesh::mBones store reference to abs. transform we multiplied with

	// process meshes
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		aiMesh *mesh = in[node->mMeshes[i]];

		// check whether we can operate on this mesh
		if (!mesh->mBones || *reinterpret_cast<aiMatrix4x4 *>(mesh->mBones) == node->mTransformation) {
			// yes, we can.
			mesh->mBones = reinterpret_cast<aiBone **>(&node->mTransformation);
			mesh->mNumBones = UINT_MAX;
			continue;
		}

		// try to find us in the list of newly created meshes
		for (unsigned int n = 0; n < out.size(); ++n) {
			aiMesh *ctz = out[n];
			if (ctz->mNumBones == node->mMeshes[i] && *reinterpret_cast<aiMatrix4x4 *>(ctz->mBones) == node->mTransformation) {

				// ok, use this one. Update node mesh index
				node->mMeshes[i] = numIn + n;
			}
		}
		if (node->mMeshes[i] < numIn) {
			// Worst case. Need to operate on a full copy of the mesh
			ASSIMP_LOG_INFO("PretransformVertices: Copying mesh due to mismatching transforms");
			aiMesh *ntz;

			const unsigned int cacheNumBones = mesh->mNumBones; //
			mesh->mNumBones = 0;
			SceneCombiner::Copy(&ntz, mesh);
			mesh->mNumBones = cacheNumBones;

			ntz->mNumBones = node->mMeshes[i];
			ntz->mBones = reinterpret_cast<aiBone **>(&node->mTransformation);

			out.push_back(ntz);

			node->mMeshes[i] = static_cast<unsigned int>(numIn + out.size() - 1);
		}
	}

	// call children
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		BuildWCSMeshes(out, in, numIn, node->mChildren[i]);
	}
}

// ------------------------------------------------------------------------------------------------
// Reset transformation matrices to identity
void PretransformVertices::MakeIdentityTransform(aiNode *nd) const {
	nd->mTransformation = aiMatrix4x4();

	// call children
	for (unsigned int i = 0; i < nd->mNumChildren; ++i) {
		MakeIdentityTransform(nd->mChildren[i]);
	}
}

// ------------------------------------------------------------------------------------------------
// Build reference counters for all meshes
void PretransformVertices::BuildMeshRefCountArray(const aiNode *nd, unsigned int *refs) const {
	for (unsigned int i = 0; i < nd->mNumMeshes; ++i)
		refs[nd->mMeshes[i]]++;

	// call children
	for (unsigned int i = 0; i < nd->mNumChildren; ++i) {
		BuildMeshRefCountArray(nd->mChildren[i], refs);
	}
}

// ------------------------------------------------------------------------------------------------
static void appendNewMeshesToScene(aiScene *pScene, std::vector<aiMesh*> &apcOutMeshes) {
	ai_assert(pScene != nullptr);

	if (apcOutMeshes.empty()) {
		return;
	}

	aiMesh **npp = new aiMesh *[pScene->mNumMeshes + apcOutMeshes.size()];

	::memcpy(npp, pScene->mMeshes, sizeof(aiMesh *) * pScene->mNumMeshes);
	::memcpy(npp + pScene->mNumMeshes, &apcOutMeshes[0], sizeof(aiMesh *) * apcOutMeshes.size());

	pScene->mNumMeshes += static_cast<unsigned int>(apcOutMeshes.size());
	delete[] pScene->mMeshes;
	pScene->mMeshes = npp;
}

// ------------------------------------------------------------------------------------------------
// Executes the post processing step on the given imported data.
void PretransformVertices::Execute(aiScene *pScene) {
	ASSIMP_LOG_DEBUG("PretransformVerticesProcess begin");

	// Return immediately if we have no meshes
	if (!pScene->mNumMeshes)
		return;

	const unsigned int oldMeshes = pScene->mNumMeshes;
	const unsigned int oldAnimationChannels = pScene->mNumAnimations;
	const unsigned int oldNodes = CountNodes(pScene->mRootNode);

	if (mConfigTransform) {
		pScene->mRootNode->mTransformation = mConfigTransformation * pScene->mRootNode->mTransformation;
	}

	// first compute absolute transformation matrices for all nodes
	ComputeAbsoluteTransform(pScene->mRootNode);

	// Delete aiMesh::mBones for all meshes. The bones are
	// removed during this step and we need the pointer as
	// temporary storage
	for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
		aiMesh *mesh = pScene->mMeshes[i];

		for (unsigned int a = 0; a < mesh->mNumBones; ++a)
			delete mesh->mBones[a];

		delete[] mesh->mBones;
        mesh->mBones = nullptr;
	}

	// now build a list of output meshes
	std::vector<aiMesh *> apcOutMeshes;

	// Keep scene hierarchy? It's an easy job in this case ...
	// we go on and transform all meshes, if one is referenced by nodes
	// with different absolute transformations a depth copy of the mesh
	// is required.
	if (mConfigKeepHierarchy) {

		// Hack: store the matrix we're transforming a mesh with in aiMesh::mBones
		BuildWCSMeshes(apcOutMeshes, pScene->mMeshes, pScene->mNumMeshes, pScene->mRootNode);

		// ... if new meshes have been generated, append them to the end of the scene
		appendNewMeshesToScene(pScene, apcOutMeshes);

		// now iterate through all meshes and transform them to world-space
		for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
			aiMatrix4x4 &mat = *reinterpret_cast<aiMatrix4x4 *>(pScene->mMeshes[i]->mBones);

			aiAnimMesh **detachedMorphs = nullptr;
			unsigned int detachedNumMorphs = 0;
			if (mConfigPreserveMorphs) {
				// FlipWindingOrderProcess reverses anim mesh stream order,
				// which would break the per-vertex correspondence between
				// replacement streams and the (not reordered) base vertices.
				// The face winding alone needs flipping, so keep the morph
				// targets out of ApplyTransform and bake them afterwards.
				detachedMorphs = pScene->mMeshes[i]->mAnimMeshes;
				detachedNumMorphs = pScene->mMeshes[i]->mNumAnimMeshes;
				pScene->mMeshes[i]->mAnimMeshes = nullptr;
				pScene->mMeshes[i]->mNumAnimMeshes = 0;
			}

			ApplyTransform(pScene->mMeshes[i], mat);

			if (mConfigPreserveMorphs) {
				pScene->mMeshes[i]->mAnimMeshes = detachedMorphs;
				pScene->mMeshes[i]->mNumAnimMeshes = detachedNumMorphs;

				// bake the same transform into the morph targets so their
				// replacement streams stay in the mesh's coordinate space
				MorphTargetBake::TransformMeshMorphs(*pScene->mMeshes[i], mat);
			}

			// prevent improper destruction
            pScene->mMeshes[i]->mBones = nullptr;
			pScene->mMeshes[i]->mNumBones = 0;
		}
	} else if (mConfigPreserveMorphs) {
		// Morph-preserving collapse: group node/mesh references by material,
		// sparse vertex format and ordered morph target layout, bake each
		// segment with its own accumulated matrix and stitch the targets.
		std::vector<unsigned int> s(pScene->mNumMeshes, 0);
		BuildMeshRefCountArray(pScene->mRootNode, &s[0]);

		std::vector<MorphBakeGroup> groups;
		FindMorphBakeGroups(pScene, pScene->mRootNode, groups);

		for (MorphBakeGroup &group : groups) {
			aiMesh *baked = AllocateBakedMesh(group);

			unsigned int curVertex = 0;
			unsigned int curFace = 0;
			CollectMorphBakeGroup(pScene, pScene->mRootNode, group, baked, &s[0], curVertex, curFace);

			baked->mMethod = group.layout->mMethod;
			MorphTargetBake::BuildStitchedMorphs(group.segments, baked->mAnimMeshes,
					baked->mNumAnimMeshes);

			apcOutMeshes.push_back(baked);
		}

		// If no meshes are referenced in the node graph it is possible that we get no output meshes.
		if (apcOutMeshes.empty()) {
			throw DeadlyImportError("No output meshes: all meshes are orphaned and are not referenced by any nodes");
		} else {
			// now delete all meshes in the scene and build a new mesh list
			for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
				aiMesh *mesh = pScene->mMeshes[i];
				mesh->mNumBones = 0;
                mesh->mBones = nullptr;

				// we're reusing the face index arrays. avoid destruction
				for (unsigned int a = 0; a < mesh->mNumFaces; ++a) {
					mesh->mFaces[a].mNumIndices = 0;
                    mesh->mFaces[a].mIndices = nullptr;
				}

				delete mesh;

				// Invalidate the contents of the old mesh array. We will most
				// likely have less output meshes now, so the last entries of
				// the mesh array are not overridden. We set them to nullptr to
				// make sure the developer gets notified when his application
				// attempts to access these fields ...
                mesh = nullptr;
			}

			// It is impossible that we have more output meshes than
			// input meshes, so we can easily reuse the old mesh array
			pScene->mNumMeshes = (unsigned int)apcOutMeshes.size();
			for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
				pScene->mMeshes[i] = apcOutMeshes[i];
			}
		}
	} else {
		apcOutMeshes.reserve(static_cast<size_t>(pScene->mNumMaterials) << 1u);
		std::list<unsigned int> aiVFormats;

		std::vector<unsigned int> s(pScene->mNumMeshes, 0);
		BuildMeshRefCountArray(pScene->mRootNode, &s[0]);

		for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
			// get the list of all vertex formats for this material
			aiVFormats.clear();
			GetVFormatList(pScene, i, aiVFormats);
			aiVFormats.sort();
			aiVFormats.unique();
			for (std::list<unsigned int>::const_iterator j = aiVFormats.begin(); j != aiVFormats.end(); ++j) {
				unsigned int numVertices = 0u;
				unsigned int numFaces = 0u;
				CountVerticesAndFaces(pScene, pScene->mRootNode, i, *j, &numFaces, &numVertices);
				if (0 != numFaces && 0 != numVertices) {
					apcOutMeshes.push_back(new aiMesh());
					aiMesh *pcMesh = apcOutMeshes.back();
					pcMesh->mNumFaces = numFaces;
					pcMesh->mNumVertices = numVertices;
					pcMesh->mFaces = new aiFace[numFaces];
					pcMesh->mVertices = new aiVector3D[numVertices];
					pcMesh->mMaterialIndex = i;
					if ((*j) & 0x2) pcMesh->mNormals = new aiVector3D[numVertices];
					if ((*j) & 0x4) {
						pcMesh->mTangents = new aiVector3D[numVertices];
						pcMesh->mBitangents = new aiVector3D[numVertices];
					}
					numFaces = 0;
					while ((*j) & (0x100 << numFaces)) {
						pcMesh->mTextureCoords[numFaces] = new aiVector3D[numVertices];
						if ((*j) & (0x10000 << numFaces)) {
							pcMesh->mNumUVComponents[numFaces] = 3;
						} else {
							pcMesh->mNumUVComponents[numFaces] = 2;
						}
						++numFaces;
					}
					numFaces = 0;
					while ((*j) & (0x1000000 << numFaces))
						pcMesh->mColors[numFaces++] = new aiColor4D[numVertices];

					// fill the mesh ...
					unsigned int aiTemp[2] = { 0, 0 };
					CollectData(pScene, pScene->mRootNode, i, *j, pcMesh, aiTemp, &s[0]);
				}
			}
		}

		// If no meshes are referenced in the node graph it is possible that we get no output meshes.
		if (apcOutMeshes.empty()) {

			throw DeadlyImportError("No output meshes: all meshes are orphaned and are not referenced by any nodes");
		} else {
			// now delete all meshes in the scene and build a new mesh list
			for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
				aiMesh *mesh = pScene->mMeshes[i];
				mesh->mNumBones = 0;
                mesh->mBones = nullptr;

				// we're reusing the face index arrays. avoid destruction
				for (unsigned int a = 0; a < mesh->mNumFaces; ++a) {
					mesh->mFaces[a].mNumIndices = 0;
                    mesh->mFaces[a].mIndices = nullptr;
				}

				delete mesh;

				// Invalidate the contents of the old mesh array. We will most
				// likely have less output meshes now, so the last entries of
				// the mesh array are not overridden. We set them to nullptr to
				// make sure the developer gets notified when his application
				// attempts to access these fields ...
                mesh = nullptr;
			}

			// It is impossible that we have more output meshes than
			// input meshes, so we can easily reuse the old mesh array
			pScene->mNumMeshes = (unsigned int)apcOutMeshes.size();
			for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
				pScene->mMeshes[i] = apcOutMeshes[i];
			}
		}
	}

	// remove all animations from the scene
	for (unsigned int i = 0; i < pScene->mNumAnimations; ++i)
		delete pScene->mAnimations[i];
	delete[] pScene->mAnimations;

	pScene->mAnimations = nullptr;
	pScene->mNumAnimations = 0;

	// --- we need to keep all cameras and lights
	for (unsigned int i = 0; i < pScene->mNumCameras; ++i) {
		aiCamera *cam = pScene->mCameras[i];
		const aiNode *nd = pScene->mRootNode->FindNode(cam->mName);
        ai_assert(nullptr != nd);

		// multiply all properties of the camera with the absolute
		// transformation of the corresponding node
		cam->mPosition = nd->mTransformation * cam->mPosition;
		cam->mLookAt = nd->mTransformation * cam->mLookAt;
		cam->mUp = aiMatrix3x3(nd->mTransformation) * cam->mUp;
	}

	for (unsigned int i = 0; i < pScene->mNumLights; ++i) {
		aiLight *l = pScene->mLights[i];
		const aiNode *nd = pScene->mRootNode->FindNode(l->mName);
        ai_assert(nullptr != nd);

		// multiply all properties of the camera with the absolute
		// transformation of the corresponding node
		l->mPosition = nd->mTransformation * l->mPosition;
		l->mDirection = aiMatrix3x3(nd->mTransformation) * l->mDirection;
		l->mUp = aiMatrix3x3(nd->mTransformation) * l->mUp;
	}

	if (!mConfigKeepHierarchy) {

		// now delete all nodes in the scene and build a new
		// flat node graph with a root node and some level 1 children
		aiNode *newRoot = new aiNode();
		newRoot->mName = pScene->mRootNode->mName;
		delete pScene->mRootNode;
		pScene->mRootNode = newRoot;

		if (1 == pScene->mNumMeshes && !pScene->mNumLights && !pScene->mNumCameras) {
			pScene->mRootNode->mNumMeshes = 1;
			pScene->mRootNode->mMeshes = new unsigned int[1];
			pScene->mRootNode->mMeshes[0] = 0;
		} else {
			pScene->mRootNode->mNumChildren = pScene->mNumMeshes + pScene->mNumLights + pScene->mNumCameras;
			aiNode **nodes = pScene->mRootNode->mChildren = new aiNode *[pScene->mRootNode->mNumChildren];

			// generate mesh nodes
			for (unsigned int i = 0; i < pScene->mNumMeshes; ++i, ++nodes) {
				aiNode *pcNode = new aiNode();
				*nodes = pcNode;
				pcNode->mParent = pScene->mRootNode;
				pcNode->mName = pScene->mMeshes[i]->mName;

				// setup mesh indices
				pcNode->mNumMeshes = 1;
				pcNode->mMeshes = new unsigned int[1];
				pcNode->mMeshes[0] = i;
			}
			// generate light nodes
			for (unsigned int i = 0; i < pScene->mNumLights; ++i, ++nodes) {
				aiNode *pcNode = new aiNode();
				*nodes = pcNode;
				pcNode->mParent = pScene->mRootNode;
				pcNode->mName.length = ai_snprintf(pcNode->mName.data, AI_MAXLEN, "light_%u", i);
				pScene->mLights[i]->mName = pcNode->mName;
			}
			// generate camera nodes
			for (unsigned int i = 0; i < pScene->mNumCameras; ++i, ++nodes) {
				aiNode *pcNode = new aiNode();
				*nodes = pcNode;
				pcNode->mParent = pScene->mRootNode;
				pcNode->mName.length = ::ai_snprintf(pcNode->mName.data, AI_MAXLEN, "cam_%u", i);
				pScene->mCameras[i]->mName = pcNode->mName;
			}
		}
	} else {
		// ... and finally set the transformation matrix of all nodes to identity
		MakeIdentityTransform(pScene->mRootNode);
	}

	if (mConfigNormalize) {
		// compute the boundary of all meshes
		aiVector3D min, max;
		MinMaxChooser<aiVector3D>()(min, max);

		for (unsigned int a = 0; a < pScene->mNumMeshes; ++a) {
			aiMesh *m = pScene->mMeshes[a];
			for (unsigned int i = 0; i < m->mNumVertices; ++i) {
				min = std::min(m->mVertices[i], min);
				max = std::max(m->mVertices[i], max);
			}
		}

		// find the dominant axis
		aiVector3D d = max - min;
		const ai_real div = std::max(d.x, std::max(d.y, d.z)) * ai_real(0.5);

		d = min + d * (ai_real)0.5;
		for (unsigned int a = 0; a < pScene->mNumMeshes; ++a) {
			aiMesh *m = pScene->mMeshes[a];
			for (unsigned int i = 0; i < m->mNumVertices; ++i) {
				m->mVertices[i] = (m->mVertices[i] - d) / div;
			}

			// The same translation and uniform scale applies to every
			// morph target; targets are not normalized on their own and
			// may extend beyond the base bounding box.
			if (mConfigPreserveMorphs) {
				for (unsigned int t = 0; t < m->mNumAnimMeshes; ++t) {
					aiAnimMesh *morph = m->mAnimMeshes[t];
					if (!morph->mVertices) {
						continue;
					}
					for (unsigned int i = 0; i < morph->mNumVertices; ++i) {
						morph->mVertices[i] = (morph->mVertices[i] - d) / div;
					}
				}
			}
		}
	}

	// print statistics
	if (!DefaultLogger::isNullLogger()) {
		ASSIMP_LOG_DEBUG("PretransformVerticesProcess finished");

		ASSIMP_LOG_INFO("Removed ", oldNodes, " nodes and ", oldAnimationChannels, " animation channels (",
				CountNodes(pScene->mRootNode), " output nodes)");
		ASSIMP_LOG_INFO("Kept ", pScene->mNumLights, " lights and ", pScene->mNumCameras, " cameras.");
		ASSIMP_LOG_INFO("Moved ", oldMeshes, " meshes to WCS (number of output meshes: ", pScene->mNumMeshes, ")");
	}
}
