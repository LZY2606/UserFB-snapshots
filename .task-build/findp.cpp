#include <cstdio>
#include <cmath>
#include <iostream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void build(fssr::IsoOctree* t,int lvl,int n,int m,float s1,float s2,int split){t->set_max_level(lvl);for(int i=1;i<n;++i)for(int j=0;j<m;++j){double a=3.141592653589793*i/n,b=6.283185307179586*j/m;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=s.normal;s.color=math::Vec3f(.2f,.5f,.8f);s.scale=(j<split?s1:s2);s.confidence=1.f;t->insert_sample(s);}t->limit_octree_level();t->compute_voxels();}
int main(){
 struct C{int lvl,n,m;float s1,s2;int sp;} cs[]={{6,16,32,0.23f,0.33f,16},{6,20,40,0.15f,0.3f,20},{7,24,48,0.08f,0.2f,24},{7,32,64,0.06f,0.15f,32},{6,24,48,0.1f,0.25f,24}};
 for(auto&c:cs){ fssr::IsoOctree t; build(&t,c.lvl,c.n,c.m,c.s1,c.s2,c.sp); auto st=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,".task-build/fp.ply",".task-build/tt",65536); std::cout<<"lvl="<<c.lvl<<" n="<<c.n<<" sc="<<c.s1<<"/"<<c.s2<<" v="<<st.vertex_count<<" t="<<st.triangle_count<<" maxp="<<st.max_polygon_vertices<<" ws="<<st.triangulation_workspace_bytes<<" passes="<<st.merge_passes<<"\n"; }
}
