#include <iostream>
#include <map>
#include "manifold/surface_partition.h"
using namespace manifold;
MeshGL64 addCube(MeshGL64 out,vec3 size,vec3 center,bool reverse){
 auto g=Manifold::Cube(size,true).Translate(center).GetMeshGL64();
 uint64_t off=out.NumVert();
 out.numProp=3;
 for(auto x:g.vertProperties) out.vertProperties.push_back(x);
 for(size_t t=0;t<g.NumTri();t++){
  uint64_t a=g.triVerts[3*t],b=g.triVerts[3*t+1],c=g.triVerts[3*t+2];
  if(!reverse){out.triVerts.push_back(off+a);out.triVerts.push_back(off+b);out.triVerts.push_back(off+c);}
  else {out.triVerts.push_back(off+a);out.triVerts.push_back(off+c);out.triVerts.push_back(off+b);}
 }
 return out;
}
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
 MeshGL64 mesh; mesh.numProp=3;
 mesh=addCube(mesh,{3,3,3},{0,0,0},false);
 mesh=addCube(mesh,{2.2,2.2,2.2},{0,0,0},true);
 mesh=addCube(mesh,{.8,.8,.8},{4,0,0},false);
 Manifold hollow(mesh);
 std::cout<<"hollow status "<<(int)hollow.Status()<<" vol "<<hollow.Volume()<<" tri "<<hollow.NumTri()<<"\n";
 auto bar=Manifold::Cube({4.5,1.1,1.1},true).Rotate(0,12,0).Translate({0,0,.2});
 test("hollow",{hollow,bar});
}
