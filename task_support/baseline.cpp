#include "storage.h"
#include "raytracing.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <unordered_set>
using namespace Cubiquity;
static bool valid(const Internals::NodeStore& nodes,u32 id,int depth,std::unordered_set<u32>& seen){
 if(id<Internals::MaterialCount)return true;
 if(id>=nodes.unsharedNodesEnd()||depth>32)return false;
 if(!seen.insert(id).second)return true;
 for(u32 child:nodes[id])if(!valid(nodes,child,depth+1,seen))return false;
 return true;
}
static int repro(){
 Volume v;v.setVoxel(1,2,3,7);v.checkpoint();v.setVoxel(1,2,3,9);
 v.undo();if(v.voxel(1,2,3)!=7)return 11;v.redo();if(v.voxel(1,2,3)!=9)return 12;
 std::puts("PRE_BAKE_UNDO_REDO_OK");
 v.bake();if(v.voxel(1,2,3)!=9)return 10;
 v.undo();std::unordered_set<u32> seen;
 bool ok=valid(Internals::getNodes(v),v.rootNodeIndex(),0,seen);
 int actual=ok?int(v.voxel(1,2,3)):-1;
 std::printf("HISTORY_AFTER_BAKE valid=%d expected=7 actual=%d\n",ok,actual);
 if(!ok||actual!=7){std::puts("REPRODUCED_STALE_HISTORY_ROOT");return 43;}
 std::puts("HISTORY_ROOT_FIXED");return 0;
}
int main(int argc,char**argv){
 if(argc==2&&std::strcmp(argv[1],"--reproduce")==0)return repro();
 Volume v;v.setVoxel(1,2,3,7);v.setVoxel(2,2,3,8);if(v.voxel(1,2,3)!=7)return 1;
 auto sub=findSubDAGs(Internals::getNodes(v),v.rootNodeIndex());
 auto hit=intersectVolume(v,sub,-3.f,2.f,3.f,1.f,.01f,.02f,true);
 if(!hit.hit||hit.material!=7||std::fabs(hit.distance-3.5)>.02)return 2;
 v.bake();if(v.voxel(1,2,3)!=7)return 3;
 sub=findSubDAGs(Internals::getNodes(v),v.rootNodeIndex());
 hit=intersectVolume(v,sub,-3.f,2.f,3.f,1.f,.01f,.02f,true);
 if(!hit.hit||hit.material!=7)return 4;
 std::puts("BASELINE_OK voxel=7 ray_material=7 baked=7");
 int r=repro();if(r!=0&&r!=43)return r;
 std::puts("CURRENT_BEHAVIOR_OBSERVED");
}
