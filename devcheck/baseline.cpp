#include "source/xatlas/xatlas.h"
#include <cassert>
#include <cmath>
#include <cstdio>
extern "C" int c_header_sanity(void);
int main(){
 assert(c_header_sanity()==1);
 float uv[]={10.25f,8.25f,14.25f,8.25f,14.25f,12.25f,10.25f,12.25f};uint32_t idx[]={0,1,2,0,2,3};
 auto a=xatlas::Create();xatlas::UvMeshDecl d;d.vertexUvData=uv;d.vertexCount=4;d.vertexStride=8;d.indexData=idx;d.indexCount=6;d.indexFormat=xatlas::IndexFormat::UInt32;
 assert(xatlas::AddUvMesh(a,d)==xatlas::AddMeshError::Success);xatlas::ComputeCharts(a);
 xatlas::PackOptions p;p.resolution=64;p.texelsPerUnit=1;p.padding=1;p.createImage=true;p.rotateCharts=false;p.rotateChartsToAxis=false;p.bruteForce=true;
 xatlas::PackCharts(a,p);assert(a->meshCount==1&&a->chartCount==1&&a->atlasCount==1&&a->image);
 double diff=0;for(unsigned i=0;i<4;i++){auto &v=a->meshes[0].vertexArray[i];assert(v.xref==i&&v.atlasIndex==0);diff+=std::fabs(v.uv[0]-uv[i*2])+std::fabs(v.uv[1]-uv[i*2+1]);}
 printf("XATLAS_UV_PACK_OK charts=%u pages=%u\nINPUT_UV_DELTA=%g\n",a->chartCount,a->atlasCount,diff);xatlas::Destroy(a);
}
