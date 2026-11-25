#include <iostream>
#include <map>
#include "manifold/surface_partition.h"
using namespace manifold;
int main(){
 auto a=Manifold::Cube({3.2,3.2,3.2},true).Rotate(11,19,7);
 auto b=Manifold::Cube({3.0,3.0,3.0},true).Rotate(27,5,31).Translate({.25,-.15,.1});
 auto c=Manifold::Cube({2.8,2.8,2.8},true).Rotate(-17,37,13).Translate({-.2,.2,.15});
 SurfacePartition p({a,b,c});
 std::cout<<"status "<<(int)p.error()<<" "<<p.errorMessage()<<" V "<<p.Vertices().size()<<" T "<<p.Triangles().size()<<"\n";
 if(!p.ok())return 1;
 for(uint64_t z: {31ULL,186ULL,191ULL}) {auto q=p.Vertices()[z]; std::cout<<"VID "<<z<<" ("<<q.x<<","<<q.y<<","<<q.z<<")\n";}
  for(auto&f:p.Triangles()) for(int z=0;z<3;z++) if(f.vertices[z]==0) {
    std::cout<<"\\nGID0FACE op"<<f.sourceOperand<<" t"<<f.sourceTriangle<<" ids "<<f.vertices[0]<<","<<f.vertices[1]<<","<<f.vertices[2]<<" mm"<<f.minusMask<<" pm"<<f.plusMask;
    break;
  }
 for(uint32_t m=1;m<8;m++){
  std::map<std::pair<uint64_t,uint64_t>,int> dir;
  std::map<std::pair<uint64_t,uint64_t>,int> und;
  int n=0;
  for(auto&f:p.Triangles()){
   bool use=f.minusMask==m || f.plusMask==m; if(!use)continue; n++;
   bool rev=f.plusMask==m;
   std::array<uint64_t,3> id=f.vertices; if(rev) std::swap(id[1],id[2]);
   for(int k=0;k<3;k++){uint64_t x=id[k],y=id[(k+1)%3]; dir[{x,y}]++; und[{std::min(x,y),std::max(x,y)}]++;}
  }
  int badCount=0,sameDir=0; std::pair<uint64_t,uint64_t> firstBad{};
  for(auto&[e,cnt]:und) if(cnt!=2){badCount++; if(badCount==1)firstBad=e;}
  for(auto&[e,cnt]:dir) if(e.first<e.second){ auto rev=dir[{e.second,e.first}]; if(cnt && rev==cnt) {} else if(cnt&&!rev){ /* one orientation list may have both? */ } }
  int doubledDirected=0; for(auto&[e,cnt]:dir) if(cnt>1) doubledDirected++;
  std::cout<<"mask "<<m<<" tris "<<n<<" badUnd "<<badCount<<" doubledDir "<<doubledDirected;
  if(badCount){auto e=firstBad; auto q0=p.Vertices()[e.first],q1=p.Vertices()[e.second]; std::cout<<" first("<<e.first<<","<<e.second<<") cnt="<<und[e]<<" p0=("<<q0.x<<","<<q0.y<<","<<q0.z<<") p1=("<<q1.x<<","<<q1.y<<","<<q1.z<<")";
   for(auto&f:p.Triangles()) for(int z=0;z<3;z++){int w=(z+1)%3; if((f.vertices[z]==e.first&&f.vertices[w]==e.second)||(f.vertices[z]==e.second&&f.vertices[w]==e.first)) std::cout<<" FACE op"<<f.sourceOperand<<" t"<<f.sourceTriangle<<" minus"<<f.minusMask<<" plus"<<f.plusMask;}
   for(size_t z=0;z<p.Vertices().size();z++){auto dv=p.Vertices()[z]-q0; double d=sqrt(dot(dv,dv)); if(d<1e-6) std::cout<<" near0_"<<z<<":"<<d;}
   for(size_t z=0;z<p.Vertices().size();z++){auto dv=p.Vertices()[z]-q1; double d=sqrt(dot(dv,dv)); if(d<1e-6) std::cout<<" near"<<z<<":"<<d;}
  }
  std::cout<<"\n";
 }
 return 0;
}
