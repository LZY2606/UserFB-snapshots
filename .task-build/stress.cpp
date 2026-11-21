#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <array>
#include <vector>
#include <functional>
#include <set>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
#include "mve/mesh.h"
static void build(fssr::IsoOctree*t,int lvl,int n,int m,float s1,float s2,int sp){t->set_max_level(lvl);for(int i=1;i<n;++i)for(int j=0;j<m;++j){double a=3.141592653589793*i/n,b=6.283185307179586*j/m;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=s.normal;s.color=math::Vec3f(.2f+.3f*(i%2),.4f,.6f);s.scale=(j<sp?s1:s2);s.confidence=.7f+.3f*((i+j)%2);t->insert_sample(s);}t->limit_octree_level();t->compute_voxels();}
struct P{float p[3];unsigned char c[3];float val,cf;};
int main(){int fails=0; for(int mode=0;mode<4;++mode) for(int sc=0;sc<3;++sc){
   int lvl=6+sc, n=20+sc*6, m=40+sc*12; float s1=0.12f-sc*0.02f, s2=0.28f-sc*0.04f;
   fssr::IsoOctree t1; build(&t1,lvl,n,m,s1,s2,m/2);
   fssr::IsoSurface iso(&t1,(fssr::InterpolationType)mode); auto mesh=iso.extract_mesh();
   auto&RV=mesh->get_vertices(); auto&RC=mesh->get_vertex_colors(); auto&RVA=mesh->get_vertex_values(); auto&RCF=mesh->get_vertex_confidences(); auto&F=mesh->get_faces();
   fssr::IsoOctree t2; build(&t2,lvl,n,m,s1,s2,m/2);
   std::string ply=".task-build/st.ply"; auto st=fssr::extract_stream_surface(&t2,(fssr::InterpolationType)mode,ply,".task-build/tt",65536);
   std::ifstream in(ply,std::ios::binary); std::string L; size_t nv=0,nf=0; while(std::getline(in,L)){if(L.rfind("element vertex ",0)==0)nv=stoul(L.substr(15));if(L.rfind("element face ",0)==0)nf=stoul(L.substr(13));if(L=="end_header")break;}
   std::vector<P> pv(nv); for(auto&v:pv){in.read((char*)v.p,12);in.read((char*)v.c,3);in.read((char*)&v.val,4);in.read((char*)&v.cf,4);} std::vector<std::array<unsigned,3>> tri(nf); for(auto&x:tri){unsigned char k;in.read((char*)&k,1);in.read((char*)x.data(),12);} 
   // attribute multiset per position
   std::map<std::array<float,3>,std::multiset<std::array<int,5>>> E,A;
   auto key=[&](float const*p){return std::array<float,3>{p[0],p[1],p[2]};};
   for(size_t i=0;i<RV.size();++i){ std::array<int,5> a; for(int k=0;k<3;++k){float s=std::min(255.f,std::max(0.f,RC[i][k]*255.f)); a[k]=(int)(s+.5f);} a[3]=*(int*)&RVA[i]; a[4]=*(int*)&RCF[i]; E[key(*RV[i])].insert(a);}
   for(size_t i=0;i<nv;++i){ std::array<int,5> a; for(int k=0;k<3;++k)a[k]=pv[i].c[k]; a[3]=*(int*)&pv[i].val; a[4]=*(int*)&pv[i].cf; A[key(pv[i].p)].insert(a);}
   bool attr = (E==A);
   bool tris=true; for(size_t t=0;t<nf && tris;++t) for(int k=0;k<3;++k){ auto a=key(pv[tri[t][k]].p), b=key(*RV[F[t*3+k]]); if(a!=b){tris=false;} }
   bool cnt = nv==RV.size() && nf==F.size()/3;
   bool ok=attr&&tris&&cnt; if(!ok)fails++;
   std::cout<<"mode="<<mode<<" sc="<<sc<<" v="<<nv<<"/"<<RV.size()<<" t="<<nf<<"/"<<F.size()/3<<" attr="<<attr<<" tris="<<tris<<" pass="<<st.merge_passes<<" peak="<<st.peak_working_bytes<<"\n";
 } std::cout<<"FAILS="<<fails<<"\n"; }
