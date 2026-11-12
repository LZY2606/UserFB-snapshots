#pragma once
#include <assimp/scene.h>
#include <memory>
inline std::unique_ptr<aiMesh> triangle(float dx=0){
 auto m=std::make_unique<aiMesh>();m->mPrimitiveTypes=aiPrimitiveType_TRIANGLE;m->mNumVertices=3;
 m->mVertices=new aiVector3D[3]{{0,0,0},{dx,0,0},{0,1,0}};
 m->mNumFaces=1;m->mFaces=new aiFace[1];m->mFaces[0].mNumIndices=3;m->mFaces[0].mIndices=new unsigned[3]{0,1,2};return m;
}
inline void addSkin(aiMesh& m){
 m.mNumBones=2;m.mBones=new aiBone*[2];
 for(unsigned i=0;i<2;++i){auto* b=m.mBones[i]=new aiBone();b->mName.Set(i?"right":"left");b->mNumWeights=2;b->mWeights=new aiVertexWeight[2]{{i,1.f},{2,.5f}};}
}
inline double skinTranslationX(const aiMesh& m,unsigned vertex){
 double result=0;for(unsigned b=0;b<m.mNumBones;++b)for(unsigned w=0;w<m.mBones[b]->mNumWeights;++w)if(m.mBones[b]->mWeights[w].mVertexId==vertex)result+=m.mBones[b]->mWeights[w].mWeight*(b?10:0);return result;
}
