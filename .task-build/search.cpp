#include <iostream>
#include "manifold/surface_partition.h"
using namespace manifold;
int main(){
 for(int aa=5;aa<=25;aa+=5) for(int bb=-20;bb<=20;bb+=10) for(int cc=5;cc<=25;cc+=10){
  auto a=Manifold::Cube({5,4.5,4},true).Rotate(aa,0,0).Translate({-.8,0,0});
  auto b=Manifold::Cube({4.5,5,4.2},true).Rotate(0,bb,0).Translate({0,-.8,0});
  auto c=Manifold::Cube({4.1,4.7,5},true).Rotate(0,0,cc).Translate({0,0,-.8});
  SurfacePartition p({a,b,c});
  if(!p.ok()){continue;}
  bool good=true; double volsum=0;
  for(uint32_t m=1;m<8;m++){SurfacePartition::Error e; auto r=p.Extract(m,&e); if(e!=SurfacePartition::Error::NoError||r.Status()!=Manifold::Error::NoError){good=false;break;} volsum+=r.Volume();}
  if(good){std::cout<<"GOOD "<<aa<<" "<<bb<<" "<<cc<<" V"<<p.Vertices().size()<<" T"<<p.Triangles().size()<<" sum"<<volsum<<"\n"; return 0;}
 }
 std::cout<<"none\n";
}
