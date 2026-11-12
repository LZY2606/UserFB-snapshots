// Added starting-point witness; no weighted optimizer is supplied.
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
static void need(bool ok, const char* what) { if (!ok) { std::fprintf(stderr,"BASELINE_FAIL %s\n",what); std::exit(1); } }
int main() {
    const unsigned short verts[] = {0,0,0, 0,0,10, 2,0,0, 2,0,10, 5,0,0, 5,0,10, 7,0,0, 7,0,10};
    // Clockwise polygons; neighbours are zero-based in creation data.
    const unsigned short polys[] = {
        0,1,3,2, 0xffff,0xffff,1,0xffff,
        2,3,5,4, 0,0xffff,2,0xffff,
        4,5,7,6, 1,0xffff,0xffff,0xffff
    };
    const unsigned short flags[] = {1,1,1};
    const unsigned char areas[] = {0,1,0};
    dtNavMeshCreateParams p; std::memset(&p,0,sizeof(p));
    p.verts=verts; p.vertCount=8; p.polys=polys; p.polyCount=3; p.nvp=4;
    p.polyFlags=flags; p.polyAreas=areas; p.bmax[0]=7; p.bmax[1]=1; p.bmax[2]=10;
    p.cs=1; p.ch=1; p.walkableHeight=2; p.walkableRadius=0; p.walkableClimb=1;
    unsigned char* data=0; int size=0;
    need(dtCreateNavMeshData(&p,&data,&size),"create");
    dtNavMesh mesh; need(dtStatusSucceed(mesh.init(data,size,DT_TILE_FREE_DATA)),"mesh init");
    dtNavMeshQuery query; need(dtStatusSucceed(query.init(&mesh,64)),"query init");
    dtQueryFilter filter; filter.setAreaCost(1,4);
    const dtPolyRef base=mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));
    const float s[]={1,0,1}, e[]={6,0,9};
    dtPolyRef path[8]; int np=0;
    need(dtStatusSucceed(query.findPath(base,base|2,s,e,&filter,path,&np,8)) && np==3,"findPath");
    float pts[24]; unsigned char outFlags[8]; dtPolyRef refs[8]; int n=0;
    need(dtStatusSucceed(query.findStraightPath(s,e,path,np,pts,outFlags,refs,&n,8,DT_STRAIGHTPATH_ALL_CROSSINGS)) && n==4,"crossings");
    double cost=0; const double weights[]={1,4,1};
    for(int i=0;i<3;++i) {
        need(std::fabs(pts[3*(i+1)]-(i==0?2:i==1?5:6))<1e-5,"portal x");
        const double dx=pts[3*i+3]-pts[3*i], dz=pts[3*i+5]-pts[3*i+2];
        cost+=weights[i]*std::sqrt(dx*dx+dz*dz);
    }
    // A fixed feasible alternative, not a reference solver or target assertion.
    const double feasible=2*std::sqrt(13.25)+4*std::sqrt(10.0);
    need(cost>26 && cost<27 && feasible<20,"existing funnel witness");
    std::printf("DETOUR_BASELINE_OK polys=%d crossings=%d funnel_cost=%.6f feasible_cost=%.6f\n",np,n,cost,feasible);
    return 0;
}
