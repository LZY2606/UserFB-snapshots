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
 auto&F=m->get_faces(); size_t nf=F.size()/3;
 std::string ply=".task-build/dbg.ply"; fssr::extract_stream_surface(&rt,fssr::INTERPOLATION_CUBIC,ply,".task-build/tt",65536);
 std::ifstream in(ply,std::ios::binary); std::string line; size_t nv=0,nf2=0; while(std::getline(in,line)){if(line.rfind("element vertex ",0)==0)nv=stoul(line.substr(15)); if(line.rfind("element face ",0)==0)nf2=stoul(line.substr(13)); if(line=="end_header")break;}
 std::vector<float> pos(nv*3); for(size_t i=0;i<nv;++i){ in.read((char*)&pos[i*3],12); in.seekg(3+4+4,std::ios::cur);} 
 std::vector<std::array<unsigned,3>> tri(nf2); for(auto&t:tri){unsigned char n;in.read((char*)&n,1);in.read((char*)t.data(),12);} 
 auto&RV=m->get_vertices();
 size_t bad=0; for(size_t t=0;t<nf;++t){ for(int k=0;k<3;++k){ unsigned id=tri[t][k]; float const*x=&pos[id*3]; auto const&r=RV[F[t*3+k]]; if(memcmp(x,&r[0],4)||memcmp(x+1,&r[1],4)||memcmp(x+2,&r[2],4)){bad++; if(bad<5) std::cout<<"tri "<<t<<" k "<<k<<" new="<<x[0]<<","<<x[1]<<","<<x[2]<<" ref="<<r[0]<<","<<r[1]<<","<<r[2]<<"\n"; break; } } }
 std::cout<<"bad tris="<<bad<<"/"<<nf<<"\n";
}
