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
struct V{float p[3]; unsigned char c[3]; float val,cf;}; 
int main(){
 fssr::IsoOctree rt; build_two(&rt); fssr::IsoSurface iso(&rt,fssr::INTERPOLATION_CUBIC); auto m=iso.extract_mesh();
 auto&RV=m->get_vertices(); auto&RC=m->get_vertex_colors(); auto&RVA=m->get_vertex_values(); auto&RCF=m->get_vertex_confidences(); auto&F=m->get_faces();
 std::string ply=".task-build/dbg.ply"; fssr::extract_stream_surface(&rt,fssr::INTERPOLATION_CUBIC,ply,".task-build/tt",65536);
 std::ifstream in(ply,std::ios::binary); std::string line; size_t nv=0,nf=0; while(std::getline(in,line)){if(line.rfind("element vertex ",0)==0)nv=stoul(line.substr(15)); if(line.rfind("element face ",0)==0)nf=stoul(line.substr(13)); if(line=="end_header")break;}
 std::vector<V> pv(nv); for(auto&v:pv){in.read((char*)v.p,12);in.read((char*)v.c,3);in.read((char*)&v.val,4);in.read((char*)&v.cf,4);} 
 std::vector<std::array<unsigned,3>> tris(nf); for(auto&t:tris){unsigned char n;in.read((char*)&n,1);in.read((char*)t.data(),12);} 
 std::map<std::array<float,3>,std::vector<size_t>> pm; for(size_t i=0;i<RV.size();++i)pm[{RV[i][0],RV[i][1],RV[i][2]}].push_back(i);
 size_t bad=0,amb=0; for(size_t i=0;i<nv;++i){auto it=pm.find({pv[i].p[0],pv[i].p[1],pv[i].p[2]}); if(it==pm.end()){bad++;continue;} if(it->second.size()==1){auto o=it->second[0]; bool ok=memcmp(&pv[i].val,&RVA[o],4)==0&&memcmp(&pv[i].cf,&RCF[o],4)==0; unsigned char cc[3]; for(int k=0;k<3;++k){float s=std::min(255.f,std::max(0.f,RC[o][k]*255.f)); cc[k]=(unsigned char)(s+0.5f);} if(!ok||memcmp(cc,pv[i].c,3)){bad++; std::cout<<"attr mismatch at "<<i<<" ref "<<o<<" val "<<pv[i].val<<"/"<<RVA[o]<<" cf "<<pv[i].cf<<"/"<<RCF[o]<<" col "<<(int)pv[i].c[0]<<"/"<<(int)cc[0]<<"\n";}} else amb++; }
 std::cout<<"nv="<<nv<<" ref="<<RV.size()<<" bad="<<bad<<" amb="<<amb<<" nf="<<nf<<" reff="<<F.size()/3<<"\n";
}
