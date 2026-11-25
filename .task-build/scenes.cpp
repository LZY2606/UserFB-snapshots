#include <iostream>
#include <map>
#include "manifold/surface_partition.h"
using namespace manifold;
void test(const char*name,std::vector<Manifold> v){
 SurfacePartition p(v);
 std::cout<<name<<" status "<<(int)p.error()<<" "<<p.errorMessage()<<" V"<<p.Vertices().size()<<" T"<<p.Triangles().size()<<"\n";
 if(!p.ok())return;
 for(uint32_t m=1;m<(1u<<v.size());m++){
  std::map<std::pair<uint64_t,uint64_t>,int> und; int n=0;
  for(auto&f:p.Triangles()){if(f.minusMask!=m&&f.plusMask!=m)continue;n++;bool rev=f.plusMask==m;auto id=f.vertices;if(rev)std::swap(id[1],id[2]);for(int k=0;k<3;k++){uint64_t x=id[k],y=id[(k+1)%3];und[{std::min(x,y),std::max(x,y)}]++;}}
  int bad=0;for(auto&e:und)if(e.second!=2)bad++;
  SurfacePartition::Error e;auto r=p.Extract(m,&e);
  std::cout<<" m"<<m<<" n"<<n<<" bad"<<bad<<" ex"<<(int)e<<" st"<<(int)r.Status()<<" vol"<<r.Volume()<<"\n";
 }
}
int main(){
 auto shell=Manifold::Cube({3,3,3},true)-Manifold::Cube({2.2,2.2,2.2},true);
 auto sep=Manifold::Cube({.8,.8,.8},true).Translate({4,0,0});
 auto hollow=shell+sep;
 auto bar=Manifold::Cube({4.5,1.1,1.1},true).Rotate(0,12,0).Translate({0,0,.2});
 test("hollow",{hollow,bar});
 auto four1=Manifold::Cube({6.0,5.0,5.2},true).Translate({-1.0,0,0});
 auto four2=Manifold::Cube({5.1,6.2,4.7},true).Translate({0,-1.0,0});
 auto four3=Manifold::Cube({4.6,5.3,6.1},true).Translate({0,0,-1.0});
 auto four4=Manifold::Cube({1.0,1.0,1.0},true).Translate({5.0,0.0,0.0});
 test("four",{four1,four2,four3,four4});
}
