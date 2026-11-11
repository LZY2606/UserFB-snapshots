// Starting-state witness only. No temporal instance implementation.
#define TINYBVH_IMPLEMENTATION
#define TINYBVH_NO_SIMD
#include "tiny_bvh.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace tinybvh;
static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "baseline assertion: %s\n", message); std::exit(1); }
}
static void pose(BLASInstance& i, float sx, float sz, float tx, float tz) {
    i.transform[0]=sx; i.transform[10]=sz;
    i.transform[3]=tx; i.transform[11]=tz;
}
static Ray probe(float x, float limit=100) {
    return Ray(bvhvec3(x,-.25f,0),bvhvec3(0,0,2),limit);
}
int main() {
    bvhvec4 vertices[] = {{-1,-1,1,0},{1,-1,1,0},{0,1,1,0}};
    BVH mesh, open, close;
    mesh.Build(vertices,1);
    BVHBase* blases[]={&mesh};
    BLASInstance a(0), b(0);
    pose(a,1,2,-4,4); pose(b,3,4,4,6);
    open.Build(&a,1,blases,1); close.Build(&b,1,blases,1);
    Ray ra=probe(-4), rb=probe(4), middle=probe(.25f);
    open.Intersect(ra); close.Intersect(rb); open.Intersect(middle);
    require(std::fabs(ra.hit.t-3)<1e-6f && std::fabs(rb.hit.t-5)<1e-6f,"static endpoint hits");
    require(middle.hit.t==100,"open pose does not cover midpoint geometry");
    require(open.IsOccluded(probe(-4,3.01f)) && !open.IsOccluded(probe(-4,2.99f)),"static finite shadow distance");
    std::printf("STATIC_ENDPOINTS open_t=%.6f close_t=%.6f shadow_limits=PASS\n",ra.hit.t,rb.hit.t);
    std::printf("STATIC_OPEN midpoint_probe=MISS\n");
    // Independently authored world-space triangle at the requested middle pose.
    // This is an ordinary static mesh, not a motion-TLAS implementation.
    bvhvec4 reference[]={{-2,-1,8,0},{2,-1,8,0},{0,1,8,0}};
    BVH world; world.Build(reference,1);
    Ray r=probe(.25f); world.Intersect(r);
    require(std::fabs(r.hit.t-4)<1e-6f && std::fabs(r.hit.u-.375f)<1e-6f && std::fabs(r.hit.v-.375f)<1e-6f,"independent midpoint geometry");
    std::printf("WORLD_MIDPOINT t=%.6f u=%.6f v=%.6f\n",r.hit.t,r.hit.u,r.hit.v);
    std::puts("STATIC_BASELINE PASS checks=5");
}
