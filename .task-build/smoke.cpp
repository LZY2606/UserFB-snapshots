#include <iostream>
#include "manifold/surface_partition.h"
using namespace manifold;
int main(){
 auto a=Manifold::Cube({3.2,3.2,3.2},true).Rotate(11,19,7);
 auto b=Manifold::Cube({3.0,3.0,3.0},true).Rotate(27,5,31).Translate({.25,-.15,.1});
 auto c=Manifold::Cube({2.8,2.8,2.8},true).Rotate(-17,37,13).Translate({-.2,.2,.15});
 SurfacePartition p({a,b,c});
 std::cout<<"status "<<(int)p.error()<<" "<<p.errorMessage()<<" V "<<p.Vertices().size()<<" T "<<p.Triangles().size()<<"\n";
 if(!p.ok())return 1;
 for(uint32_t m=1;m<8;m++){SurfacePartition::Error e; auto r=p.Extract(m,&e); std::cout<<"mask "<<m<<" e "<<(int)e<<" st "<<(int)r.Status()<<" empty "<<r.IsEmpty()<<" vol "<<r.Volume()<<" tris "<<r.NumTri()<<"\n";}
 return 0;
}
