#include <iostream>
#include <map>
#include "manifold/surface_partition.h"
using namespace manifold;
int main(){
 SimplePolygon sp;
 for(auto p: {vec2(-2,-1),vec2(2,-1),vec2(2,0),vec2(0,0),vec2(0,1),vec2(-2,1)}) sp.push_back(p);
 Manifold L=Manifold::Extrude({sp},2).Translate({0,0,-1});
 auto cube=Manifold::Cube({2.5,2.5,2.5},true).Translate({.4,0,0});
 SurfacePartition p({L,cube});
 std::cout<<"Lst "<<(int)L.Status()<<" vol"<<L.Volume()<<" pst "<<(int)p.error()<<" "<<p.errorMessage()<<" T"<<p.Triangles().size()<<"\n";
 for(uint32_t m=1;m<4;m++){
  std::map<std::pair<uint64_t,uint64_t>,int> und;int n=0;
  for(auto&f:p.Triangles()){if(f.minusMask!=m&&f.plusMask!=m)continue;n++;bool rev=f.plusMask==m;auto id=f.vertices;if(rev)std::swap(id[1],id[2]);for(int k=0;k<3;k++){uint64_t x=id[k],y=id[(k+1)%3];und[{std::min(x,y),std::max(x,y)}]++;}}
  int bad=0;for(auto&e:und)if(e.second!=2)bad++;
  SurfacePartition::Error e;auto r=p.Extract(m,&e);std::cout<<"m"<<m<<" n"<<n<<" bad"<<bad<<" ex"<<(int)e<<" st"<<(int)r.Status()<<" vol"<<r.Volume()<<"\n";
 }
}
