#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include <cassert>
#include <cstdio>
int main(){
 unsigned short v[]={0,0,0, 0,0,10, 10,0,10, 10,0,0, 20,0,10, 20,0,0};
 unsigned short p[]={0,1,2,3, 0xffff,0xffff,1,0xffff, 3,2,4,5, 0,0xffff,0xffff,0xffff};
 unsigned short flags[]={1,1};unsigned char areas[]={0,0};
 dtNavMeshCreateParams cp={};cp.verts=v;cp.vertCount=6;cp.polys=p;cp.polyCount=2;cp.nvp=4;cp.polyFlags=flags;cp.polyAreas=areas;
 cp.cs=cp.ch=1;cp.bmax[0]=20;cp.bmax[1]=2;cp.bmax[2]=10;cp.walkableHeight=2;cp.walkableRadius=.1f;cp.walkableClimb=.5f;
 unsigned char* data=nullptr;int size=0;assert(dtCreateNavMeshData(&cp,&data,&size));
 dtNavMesh mesh;assert(dtStatusSucceed(mesh.init(data,size,DT_TILE_FREE_DATA)));
 dtNavMeshQuery query;assert(dtStatusSucceed(query.init(&mesh,128)));dtQueryFilter filter;
 auto base=mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));dtPolyRef path[8];int n=0;
 float a[]={2,0,5},b[]={18,0,5};assert(dtStatusSucceed(query.findPath(base,base+1,a,b,&filter,path,&n,8)));assert(n==2);
 float pts[24];int count=0;assert(dtStatusSucceed(query.findStraightPath(a,b,path,n,pts,nullptr,nullptr,&count,8)));assert(count==2);
 printf("BASELINE_DETOUR_PASS corridor=%d corners=%d\n",n,count);
}
