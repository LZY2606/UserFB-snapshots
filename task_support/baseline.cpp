#include "scene_api.hpp"
#include <cstdio>
using namespace tinyscene;
static bool hit(float x) {
    tinybvh::Ray r(tinybvh::bvhvec3(x,0,0),tinybvh::bvhvec3(0,0,1));
    Scene::tlas->Intersect(r);
    return r.hit.t<100;
}
int main() {
    const int mat=Scene::AddMaterial(ts_vec3(.4f),"normal");
    const int mesh=Scene::AddMesh(0);
    Scene::AddTriToMesh(mesh,ts_vec3(-1,-1,4),ts_vec3(1,-1,4),ts_vec3(0,1,4),mat);
    Scene::meshPool[mesh]->BuildMaterialList();
    ts_mat4 a,b; b[3]=4;
    int left=Scene::AddNode(new Node(mesh,a));
    int right=Scene::AddNode(new Node(mesh,b));
    Scene::AddInstance(left); Scene::AddInstance(right);
    Scene::UpdateSceneGraph(0);
    if(!hit(0)||!hit(4))return 2;
    std::printf("BASELINE_NORMAL instances=%zu left_hit=1 right_hit=1\n",Scene::instPool.size());
    Scene::RemoveNode(left);
    Scene::UpdateSceneGraph(0);
    const bool ghost=hit(0),live=hit(4);
    std::printf("BASELINE_AFTER_REMOVE roots=%zu instances=%zu deleted_hit=%d survivor_hit=%d\n",Scene::rootNodes.size(),Scene::instPool.size(),ghost,live);
    if(Scene::rootNodes.size()!=1||!live)return 3;
    if(ghost) std::puts("BASELINE_REPRODUCED_GHOST"); else std::puts("BASELINE_GHOST_FIXED");
    return 0;
}
