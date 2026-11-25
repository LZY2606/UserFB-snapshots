#include <iostream>
#include <map>
#include "manifold/surface_partition.h"
using namespace manifold;
int main(){
 auto a=Manifold::Cube({6,5,5},true).Translate({-1,0,0});
 auto b=Manifold::Cube({5,6,5.5},true).Translate({0,-1,0});
 auto c=Manifold::Cube({4.5,5.5,6},true).Translate({0,0,-1});
 SurfacePartition p({a,b,c});
 std::cout<<"status "<<(int)p.error()<<" "<<p.errorMessage()<<" V "<<p.Vertices().size()<<" T "<<p.Triangles().size()<<"\n";
 if(!p.ok())return 1;
 for(uint32_t m=1;m<8;m++){
  std::map<std::pair<uint64_t,uint64_t>,int> und; int n=0;
  for(auto&f:p.Triangles()){bool use=f.minusMask==m||f.plusMask==m;if(!use)continue;n++;bool rev=f.plusMask==m;auto id=f.vertices;if(rev)std::swap(id[1],id[2]);for(int k=0;k<3;k++){uint64_t x=id[k],y=id[(k+1)%3];und[{std::min(x,y),std::max(x,y)}]++;}}
  int bad=0; for(auto&e:und)if(e.second!=2)bad++;
  SurfacePartition::Error er; auto r=p.Extract(m,&er);
  std::cout<<"m"<<m<<" n"<<n<<" bad"<<bad<<" ex"<<(int)er<<" st"<<(int)r.Status()<<" vol"<<r.Volume()<<" tri"<<r.NumTri()<<"\n";
 }
}
