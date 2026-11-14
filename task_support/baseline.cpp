#include <assimp/scene.h>
#include <assimp/SceneCombiner.h>
#include <assimp/Importer.hpp>
#include "PostProcessing/PretransformVertices.h"
#include <cmath>
#include <iostream>
#include <memory>
static aiMesh *triangle(bool morph) {
    auto *m=new aiMesh;
    m->mPrimitiveTypes=aiPrimitiveType_TRIANGLE;
    m->mNumVertices=3; m->mVertices=new aiVector3D[3]{{1,0,0},{2,0,0},{1,1,0}};
    m->mNumFaces=1; m->mFaces=new aiFace[1];
    m->mFaces[0].mNumIndices=3; m->mFaces[0].mIndices=new unsigned[3]{0,1,2};
    if(morph) {
        m->mNumAnimMeshes=1; m->mAnimMeshes=new aiAnimMesh*[1]{new aiAnimMesh};
        auto *a=m->mAnimMeshes[0]; a->mName.Set("smile"); a->mWeight=.25f;
        a->mNumVertices=3; a->mVertices=new aiVector3D[3]{{2,0,0},{3,0,0},{2,1,0}};
    }
    return m;
}
static std::unique_ptr<aiScene> scene() {
    auto s=std::make_unique<aiScene>(); s->mNumMeshes=1;s->mMeshes=new aiMesh*[1]{triangle(true)};
    s->mNumMaterials=1;s->mMaterials=new aiMaterial*[1]{new aiMaterial};
    s->mRootNode=new aiNode; auto *r=s->mRootNode;
    r->mNumMeshes=1;r->mMeshes=new unsigned[1]{0};r->mTransformation.a4=10;
    r->mNumChildren=1;r->mChildren=new aiNode*[1]{new aiNode};
    auto *c=r->mChildren[0];c->mParent=r;c->mNumMeshes=1;c->mMeshes=new unsigned[1]{0};c->mTransformation.a4=20;
    return s;
}
int main() {
    for(bool keep:{true,false}) {
        auto s=scene(); Assimp::Importer settings;
        Assimp::PretransformVertices p;p.SetupProperties(&settings);p.KeepHierarchy(keep);p.Execute(s.get());
        if(s->mNumMeshes!=(keep?2u:1u)||std::abs(s->mMeshes[0]->mVertices[0].x-11)>1e-6)return 2;
        auto *m=s->mMeshes[0];
        std::cout<<(keep?"KEEP":"COLLAPSE")<<" base_x="<<m->mVertices[0].x<<" targets="<<m->mNumAnimMeshes;
        if(m->mNumAnimMeshes)std::cout<<" target_x="<<m->mAnimMeshes[0]->mVertices[0].x;
        std::cout<<'\n';
    }
    std::unique_ptr<aiMesh> a(triangle(true)),b(triangle(true));std::vector<aiMesh*> in{a.release(),b.release()};
    aiMesh *raw=nullptr;Assimp::SceneCombiner::MergeMeshes(&raw,0,in.begin(),in.end());std::unique_ptr<aiMesh> out(raw);
    if(!out||out->mNumVertices!=6||out->mNumFaces!=2)return 3;
    std::cout<<"MERGE vertices="<<out->mNumVertices<<" targets="<<out->mNumAnimMeshes<<"\nBASELINE_OK\n";
}
