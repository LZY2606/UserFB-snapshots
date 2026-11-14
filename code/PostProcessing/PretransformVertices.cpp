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
#include "ProcessHelper.h"
#include "Common/MorphTargetUtils.h"
#include <assimp/Exceptional.h>
#include <assimp/SceneCombiner.h>

#include <algorithm>

using namespace Assimp;

// some array offsets
#define AI_PTVS_VERTEX 0x0
#define AI_PTVS_FACE 0x1

namespace {

// Get a bitwise combination identifying the vertex format of a mesh.
//
// When sparse channels are preserved, every populated slot is reported, even
// if an earlier slot is empty. Otherwise the historical contiguous-prefix
// behaviour of GetMeshVFormatUnique() is kept so that disabling the morph
// option does not alter existing output.
static unsigned int ComputeMeshVFormat(const aiMesh *pcMesh, bool preserveSparse) {
	if (!preserveSparse) {
		return GetMeshVFormatUnique(pcMesh);
	}

	unsigned int iRet = 1;
	if (pcMesh->HasNormals()) iRet |= 0x2;
	if (pcMesh->HasTangentsAndBitangents()) iRet |= 0x4;

	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++p) {
		if (pcMesh->HasTextureCoords(p)) {
			iRet |= (0x100u << p);
			if (3 == pcMesh->mNumUVComponents[p]) {
				iRet |= (0x10000u << p);
			}
		}
	}
	for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_COLOR_SETS; ++p) {
		if (pcMesh->HasVertexColors(p)) {
			iRet |= (0x1000000u << p);
		}
	}
	return iRet;
}

// Get a bitwise combination identifying the vertex format of a mesh
static unsigned int GetMeshVFormat(aiMesh *pcMesh, bool preserveSparse)  {
	// the vertex format is stored in aiMesh::mBones for later retrieval.
	// there isn't a good reason to compute it a few hundred times
	// from scratch. The pointer is unused as animations are lost
	// during PretransformVertices.
	if (pcMesh->mBones)
		return (unsigned int)(uint64_t)pcMesh->mBones;

	const unsigned int iRet = ComputeMeshVFormat(pcMesh, preserveSparse);

	// store the value for later use
	pcMesh->mBones = (aiBone **)(uint64_t)iRet;
	return iRet;
}

// Get the list of unique output specs that occur for a given material index.
static void GetMorphSpecs(const aiScene *pcScene, unsigned int iMat, bool preserve,
		std::vector<MorphTargetSpec> &aiOut) {
	for (unsigned int i = 0; i < pcScene->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[i];
		if (iMat != pcMesh->mMaterialIndex) {
			continue;
		}

		MorphTargetSpec spec;
		spec.mVertexFormat = GetMeshVFormat(pcMesh, preserve);
		if (preserve) {
			spec.mLayout = GetMorphLayout(pcMesh);
		}

		bool found = false;
		for (std::vector<MorphTargetSpec>::const_iterator it = aiOut.begin(); it != aiOut.end(); ++it) {
			if (it->mVertexFormat != spec.mVertexFormat) {
				continue;
			}
			if (preserve && !MorphLayoutsEqual(it->mLayout, spec.mLayout)) {
				continue;
			}
			found = true;
			break;
		}
		if (!found) {
			aiOut.push_back(spec);
		}
	}

	// Keep the historical output ordering: ascending vertex format. Only ties
	// (several target layouts sharing a format) keep their discovery order.
	std::stable_sort(aiOut.begin(), aiOut.end(),
			[](const MorphTargetSpec &a, const MorphTargetSpec &b) {
				return a.mVertexFormat < b.mVertexFormat;
			});
}

// Check whether a mesh belongs to an output spec.
static bool MeshMatchesSpec(aiMesh *pcMesh, unsigned int iMat, const MorphTargetSpec &spec, bool preserve) {
	if (iMat != pcMesh->mMaterialIndex) {
		return false;
	}
	if (spec.mVertexFormat != GetMeshVFormat(pcMesh, preserve)) {
		return false;
	}
	if (preserve && !MeshMatchesMorphLayout(pcMesh, spec.mLayout)) {
		return false;
	}
	return true;
}

// Flip the winding order of all faces without touching any vertex stream.
// Used when baking a reflective transform and morph targets are kept: the
// vertex order of the base mesh and of every target must stay aligned.
static void FlipFaceWinding(aiMesh *pcMesh) {
	for (unsigned int a = 0; a < pcMesh->mNumFaces; ++a) {
		aiFace &face = pcMesh->mFaces[a];
		for (unsigned int b = 0; b < face.mNumIndices / 2; ++b) {
			std::swap(face.mIndices[b], face.mIndices[face.mNumIndices - 1 - b]);
		}
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
	// AI_CONFIG_PP_PTV_ADD_ROOT_TRANSFORMATION and AI_CONFIG_PP_PTV_ROOT_TRANSFORMATION
	mConfigKeepHierarchy = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_KEEP_HIERARCHY, 0));
	mConfigNormalize = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_NORMALIZE, 0));
	mConfigTransform = (0 != pImp->GetPropertyInteger(AI_CONFIG_PP_PTV_ADD_ROOT_TRANSFORMATION, 0));

	mConfigTransformation = pImp->GetPropertyMatrix(AI_CONFIG_PP_PTV_ROOT_TRANSFORMATION, aiMatrix4x4());

	mConfigPointCloud = pImp->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);
	mConfigPreserveMorphs = pImp->GetPropertyBool(AI_CONFIG_PP_PTV_PRESERVE_MORPHS, false);
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
		const MorphTargetSpec &spec, unsigned int *piFaces, unsigned int *piVertices) const {
	for (unsigned int i = 0; i < pcNode->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[pcNode->mMeshes[i]];
		if (MeshMatchesSpec(pcMesh, iMat, spec, mConfigPreserveMorphs)) {
			*piVertices += pcMesh->mNumVertices;
			*piFaces += pcMesh->mNumFaces;
		}
	}
	for (unsigned int i = 0; i < pcNode->mNumChildren; ++i) {
		CountVerticesAndFaces(pcScene, pcNode->mChildren[i], iMat, spec, piFaces, piVertices);
	}
}

// ------------------------------------------------------------------------------------------------
// Collect vertex/face data
void PretransformVertices::CollectData(const aiScene *pcScene, const aiNode *pcNode, unsigned int iMat,
		const MorphTargetSpec &spec, aiMesh *pcMeshOut,
		unsigned int aiCurrent[2], unsigned int *num_refs,
		std::vector<MorphSource> *morphSources) const {
	const unsigned int iVFormat = spec.mVertexFormat;
	// No need to multiply if there's no transformation
	const bool identity = pcNode->mTransformation.IsIdentity();
	for (unsigned int i = 0; i < pcNode->mNumMeshes; ++i) {
		aiMesh *pcMesh = pcScene->mMeshes[pcNode->mMeshes[i]];
		if (MeshMatchesSpec(pcMesh, iMat, spec, mConfigPreserveMorphs)) {
			// Remember the source and the transform used for this instance so
			// that target streams can be baked without touching the source.
			if (mConfigPreserveMorphs && morphSources) {
				MorphSource source;
				source.mesh = pcMesh;
				source.transform = pcNode->mTransformation;
				morphSources->push_back(source);
			}
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
			// Copy every populated texture coordinate and colour channel. All
			// slots are checked (not just a contiguous prefix) so that sparse
			// channel layouts survive baking. When morph preservation is off
			// the format mask only contains the contiguous prefix, so this is
			// equivalent to the historical loops.
			for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++p) {
				if ((iVFormat & (0x100u << p)) && pcMeshOut->mTextureCoords[p] && pcMesh->mTextureCoords[p]) {
					memcpy(pcMeshOut->mTextureCoords[p] + aiCurrent[AI_PTVS_VERTEX],
							pcMesh->mTextureCoords[p],
							pcMesh->mNumVertices * sizeof(aiVector3D));
				}
			}
			for (unsigned int p = 0; p < AI_MAX_NUMBER_OF_COLOR_SETS; ++p) {
				if ((iVFormat & (0x1000000u << p)) && pcMeshOut->mColors[p] && pcMesh->mColors[p]) {
					memcpy(pcMeshOut->mColors[p] + aiCurrent[AI_PTVS_VERTEX],
							pcMesh->mColors[p],
							pcMesh->mNumVertices * sizeof(aiColor4D));
				}
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
				spec, pcMeshOut, aiCurrent, num_refs, morphSources);
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
		if (mConfigPreserveMorphs) {
			// Flip only the face winding. The historical helper also reverses
			// every array of the attached anim meshes, which would push target
			// vertices out of sync with the base mesh.
			FlipFaceWinding(mesh);
		} else {
			// Reverse the mesh face winding order
			FlipWindingOrderProcess::ProcessMesh(mesh);
		}
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

	// Bake the morph targets with the very same cumulative matrix as the
	// host mesh.
	if (mConfigPreserveMorphs) {
		BakeMorphTargets(mesh, mat);
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
			ApplyTransform(pScene->mMeshes[i], *reinterpret_cast<aiMatrix4x4 *>(pScene->mMeshes[i]->mBones));

			// prevent improper destruction
            pScene->mMeshes[i]->mBones = nullptr;
			pScene->mMeshes[i]->mNumBones = 0;
		}
	} else {
		apcOutMeshes.reserve(static_cast<size_t>(pScene->mNumMaterials) << 1u);

		for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
			// get the list of all output specs for this material
			std::vector<MorphTargetSpec> aiSpecs;
			GetMorphSpecs(pScene, i, mConfigPreserveMorphs, aiSpecs);
			for (std::vector<MorphTargetSpec>::const_iterator j = aiSpecs.begin(); j != aiSpecs.end(); ++j) {
				const unsigned int iVFormat = j->mVertexFormat;
				unsigned int numVertices = 0u;
				unsigned int numFaces = 0u;
				CountVerticesAndFaces(pScene, pScene->mRootNode, i, *j, &numFaces, &numVertices);
				if (0 == numFaces || 0 == numVertices) {
					continue;
				}

				// Each bucket needs its own reference counts; CollectData
				// consumes the entries of this mesh for the faces it steals.
				std::vector<unsigned int> bucketRefs(pScene->mNumMeshes, 0);
				BuildMeshRefCountArray(pScene->mRootNode, &bucketRefs[0]);

				apcOutMeshes.push_back(new aiMesh());
				aiMesh *pcMesh = apcOutMeshes.back();
				pcMesh->mNumFaces = numFaces;
				pcMesh->mNumVertices = numVertices;
				pcMesh->mFaces = new aiFace[numFaces];
				pcMesh->mVertices = new aiVector3D[numVertices];
				pcMesh->mMaterialIndex = i;
				pcMesh->mMethod = j->mLayout.mMethod;
				if (iVFormat & 0x2) pcMesh->mNormals = new aiVector3D[numVertices];
				if (iVFormat & 0x4) {
					pcMesh->mTangents = new aiVector3D[numVertices];
					pcMesh->mBitangents = new aiVector3D[numVertices];
				}
				for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++n) {
					if (iVFormat & (0x100u << n)) {
						pcMesh->mTextureCoords[n] = new aiVector3D[numVertices];
						if (iVFormat & (0x10000u << n)) {
							pcMesh->mNumUVComponents[n] = 3;
						} else {
							pcMesh->mNumUVComponents[n] = 2;
						}
					}
				}
				for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_COLOR_SETS; ++n) {
					if (iVFormat & (0x1000000u << n)) {
						pcMesh->mColors[n] = new aiColor4D[numVertices];
					}
				}

				// fill the mesh ...
				unsigned int aiTemp[2] = { 0, 0 };
				std::vector<MorphSource> morphSources;
				CollectData(pScene, pScene->mRootNode, i, *j, pcMesh, aiTemp, &bucketRefs[0],
						mConfigPreserveMorphs ? &morphSources : nullptr);

				if (mConfigPreserveMorphs && !morphSources.empty()) {
					std::vector<const aiMesh *> inputs;
					std::vector<aiMatrix4x4> transforms;
					inputs.reserve(morphSources.size());
					transforms.reserve(morphSources.size());
					for (std::vector<MorphSource>::const_iterator src = morphSources.begin(); src != morphSources.end(); ++src) {
						inputs.push_back(src->mesh);
						transforms.push_back(src->transform);
					}
					MergeMorphTargets(pcMesh, inputs, &transforms);
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
		// compute the boundary of all base meshes
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

			// The mapping is derived from the baked base geometry only and
			// then applied unchanged to every target position. Targets are
			// allowed to end up outside the base bounding box.
			if (mConfigPreserveMorphs && m->mAnimMeshes) {
				for (unsigned int t = 0; t < m->mNumAnimMeshes; ++t) {
					aiAnimMesh *target = m->mAnimMeshes[t];
					if (target == nullptr || target->mVertices == nullptr) {
						continue;
					}
					for (unsigned int i = 0; i < target->mNumVertices; ++i) {
						target->mVertices[i] = (target->mVertices[i] - d) / div;
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
