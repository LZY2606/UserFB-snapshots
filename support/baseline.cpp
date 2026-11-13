#include "src/meshoptimizer.h"
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <utility>
int main(){
 const float p[]={1,1,1,-1,-1,1,-1,1,-1,1,-1,-1};
 const unsigned int idx[]={0,2,1,0,1,3,0,3,2,1,2,3};
 meshopt_Meshlet m[4]={};unsigned int vertices[12]={};unsigned char triangles[36]={};
 size_t n=meshopt_buildMeshlets(m,vertices,triangles,idx,12,p,4,12,64,124,0);
 assert(n==1&&m[0].triangle_count==4&&m[0].vertex_count==4);
 std::map<std::pair<unsigned,unsigned>,unsigned> edges;
 for(unsigned t=0;t<4;t++)for(unsigned j=0;j<3;j++){
 unsigned a=triangles[m[0].triangle_offset+t*3+j],b=triangles[m[0].triangle_offset+t*3+(j+1)%3];if(a>b)std::swap(a,b);++edges[{a,b}];}
 unsigned boundary=0;for(auto e:edges){assert(e.second==2);if(e.second==1)boundary++;}
 int chi=int(m[0].vertex_count)-int(edges.size())+int(m[0].triangle_count);
 assert(chi==2&&boundary==0);
 std::cout<<"TETRA meshlets="<<n<<" V=4 E="<<edges.size()<<" F=4 chi="<<chi<<" boundary="<<boundary<<" disk=no\n";
 std::cout<<"SOURCE_TRIANGLES preserved=4 production-clusterizer=ok\n";
}
