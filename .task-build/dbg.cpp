#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <array>
#include <set>
#include <vector>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
#include "mve/mesh.h"
static void build_two(fssr::IsoOctree* tree){tree->set_max_level(6);float gap=1.0f;for(int s=0;s<2;++s){math::Vec3f c(s==0?-gap/2:gap/2,0,0);for(int i=1;i<20;++i){double a=M_PI*i/20;for(int j=0;j<40;++j){double b=2*M_PI*j/40;math::Vec3f d(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));fssr::Sample sm;sm.normal=d;sm.pos=c+d*0.4f;sm.color=math::Vec3f(s==0?0.9f:0.1f,0.5f,0.2f);sm.scale=0.08f;sm.confidence=1;tree->insert_sample(sm);}}}tree->limit_octree_level();tree->compute_voxels();}
int main(){
 fssr::IsoOctree rt; build_two(&rt); fssr::IsoSurface iso(&rt,fssr::INTERPOLATION_CUBIC); auto m=iso.extract_mesh();
 auto&V=m->get_vertices(); auto&F=m->get_faces();
 std::map<std::array<float,3>,std::vector<size_t>> pm; for(size_t i=0;i<V.size();++i) pm[{V[i][0],V[i][1],V[i][2]}].push_back(i);
 size_t dup=0,dupmulti=0; for(auto&kv:pm){if(kv.second.size()>1){dup+=kv.second.size();dupmulti++;}}
 std::cout<<"ref v="<<V.size()<<" t="<<F.size()/3<<" unique_pos="<<pm.size()<<" dup_vertices="<<dup<<" groups="<<dupmulti<<"\n";
 // check edge key uniqueness: impossible from mesh; check nan
 size_t nan=0; for(auto&v:V) for(int d=0;d<3;++d) if(!std::isfinite(v[d])) nan++;
 std::cout<<"nan="<<nan<<"\n";
 std::string ply=".task-build/dbg.ply"; auto st=fssr::extract_stream_surface(&rt,fssr::INTERPOLATION_CUBIC,ply,".task-build/tt",65536);
 std::cout<<"new v="<<st.vertex_count<<" t="<<st.triangle_count<<" maxp="<<st.max_polygon_vertices<<"\n";
}
