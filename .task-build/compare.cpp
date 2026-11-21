#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <array>
#include <fstream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
#include "fssr/hermite.h"
#include "mve/mesh.h"

struct Ply { std::vector<std::array<float,3>> pos; std::vector<std::array<unsigned char,3>> col; std::vector<float> val, conf; std::vector<std::array<unsigned,3>> faces; };
static Ply load(const std::string& fn){
  Ply p; std::ifstream in(fn,std::ios::binary); std::string line; size_t nv=0,nf=0;
  while(std::getline(in,line)){ if(line.rfind("element vertex ",0)==0) nv=stoul(line.substr(15)); if(line.rfind("element face ",0)==0) nf=stoul(line.substr(13)); if(line=="end_header") break; }
  for(size_t i=0;i<nv;++i){ float x[3]; unsigned char c[3]; float v,cf; in.read((char*)x,12); in.read((char*)c,3); in.read((char*)&v,4); in.read((char*)&cf,4); p.pos.push_back({x[0],x[1],x[2]}); p.col.push_back({c[0],c[1],c[2]}); p.val.push_back(v); p.conf.push_back(cf);} 
  for(size_t i=0;i<nf;++i){ unsigned char n; unsigned int idx[3]; in.read((char*)&n,1); in.read((char*)idx,12); p.faces.push_back({idx[0],idx[1],idx[2]});} return p;
}
static void build(fssr::IsoOctree* tree,int lvl,int ni,int nj,float sc,int mode){ tree->set_max_level(lvl); for(int i=1;i<ni;++i)for(int j=0;j<nj;++j){double a=3.141592653589793*i/ni,b=6.283185307179586*j/nj;fssr::Sample s; s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a)); s.pos=s.normal; s.color=math::Vec3f(.2f,.5f,.8f); s.scale=(j<nj/2?sc:sc*1.4f); s.confidence=1.f; tree->insert_sample(s);} tree->limit_octree_level(); tree->compute_voxels(); }
static bool feq(float a,float b){return memcmp(&a,&b,4)==0;}
int main(int argc,char**argv){
  int lvl=6,ni=16,nj=32; float sc=.23f;
  for(int mode=0;mode<4;++mode){
    fssr::IsoOctree t1; build(&t1,lvl,ni,nj,sc,0);
    fssr::IsoSurface iso(&t1,(fssr::InterpolationType)mode);
    auto m=iso.extract_mesh();
    fssr::IsoOctree t2; build(&t2,lvl,ni,nj,sc,0);
    std::string ply=".task-build/cmp"+std::to_string(mode)+".ply";
    auto st=fssr::extract_stream_surface(&t2,(fssr::InterpolationType)mode,ply,".task-build/tt",65536);
    Ply p=load(ply);
    auto const& v=m->get_vertices(); auto const& f=m->get_faces(); auto const& c=m->get_vertex_colors(); auto const& val=m->get_vertex_values(); auto const& cf=m->get_vertex_confidences();
    if(v.size()!=p.pos.size()||f.size()!=p.faces.size()*3){ std::cout<<"mode "<<mode<<" size mismatch old="<<v.size()<<","<<f.size()/3<<" new="<<p.pos.size()<<","<<p.faces.size()<<" st="<<st.vertex_count<<","<<st.triangle_count<<"\n"; continue; }
    // map new id -> old index by exact position+value+conf
    std::map<std::array<float,3>, size_t> bypos; // may collide
    std::vector<std::vector<size_t>> candidates(v.size());
    std::map<std::array<float,3>, std::vector<size_t>> mm;
    for(size_t i=0;i<v.size();++i) mm[{v[i][0],v[i][1],v[i][2]}].push_back(i);
    std::vector<long> mapid(p.pos.size(),-1); size_t unmatched=0;
    for(size_t i=0;i<p.pos.size();++i){ auto it=mm.find({p.pos[i][0],p.pos[i][1],p.pos[i][2]}); if(it==mm.end()||it->second.size()!=1){ unmatched++; continue;} mapid[i]=it->second[0]; }
    size_t badpos=0,badattr=0,badtri=0,badadj=0;
    for(size_t i=0;i<v.size();++i) if(mapid[i]>=0){ auto o=mapid[i]; if(!feq(v[o][0],p.pos[i][0])||!feq(v[o][1],p.pos[i][1])||!feq(v[o][2],p.pos[i][2])) badpos++; }
    // attributes
    for(size_t i=0;i<v.size();++i) if(mapid[i]>=0){ auto o=mapid[i]; if(!feq(val[o],p.val[i])||!feq(cf[o],p.conf[i])) badattr++; }
    // oriented triangles in sequence
    for(size_t t=0;t<p.faces.size();++t){ auto nf=p.faces[t]; long a=mapid[nf[0]],b=mapid[nf[1]],cc=mapid[nf[2]]; if(a<0||b<0||cc<0){badtri++;continue;} if(f[3*t]!=(size_t)a||f[3*t+1]!=(size_t)b||f[3*t+2]!=(size_t)cc) badtri++; }
    std::cout<<"mode "<<mode<<" old="<<v.size()<<"/"<<f.size()/3<<" new="<<p.pos.size()<<"/"<<p.faces.size()<<" unmatched="<<unmatched<<" badpos="<<badpos<<" badattr="<<badattr<<" badtri="<<badtri<<" peak="<<st.peak_working_bytes<<" maxp="<<st.max_polygon_vertices<<"\n";
  }
  return 0;
}
