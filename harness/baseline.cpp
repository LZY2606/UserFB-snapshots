#include "fixture.h"
#include "PostProcessing/JoinVerticesProcess.h"
#include <cassert>
#include <iostream>
int main(){
 Assimp::JoinVerticesProcess process;
 auto plain=triangle();process.ProcessMesh(plain.get(),0);assert(plain->mNumVertices==2);
 auto near=triangle(0.000005f);process.ProcessMesh(near.get(),0);
 auto skin=triangle();addSkin(*skin);double before=skinTranslationX(*skin,1);assert(before==10);
 process.ProcessMesh(skin.get(),0);unsigned remapped=skin->mFaces[0].mIndices[1];double after=skinTranslationX(*skin,remapped);
 assert(skin->mFaces[0].mIndices[2]<skin->mNumVertices);
 std::cout<<"ASSIMP_EXACT_STATIC_OK count="<<plain->mNumVertices<<"\n";
 std::cout<<"ASSIMP_NEAR_CURRENT count="<<near->mNumVertices<<" expected=2\n";
 std::cout<<"ASSIMP_SKIN_CURRENT before="<<before<<" after="<<after<<" expected=10\n";
}
