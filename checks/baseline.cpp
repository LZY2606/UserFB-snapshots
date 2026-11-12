#include "nanort.h"
#include <cassert>
#include <cstdio>
int main(){float v[]={0,0,2,2,0,2,0,2,2,2,2,2};unsigned f[]={0,1,2,1,3,2};
 nanort::TriangleMesh<float> mesh(v,f,3*sizeof(float));nanort::TriangleSAHPred<float> pred(v,f,3*sizeof(float));nanort::BVHAccel<float> bvh;assert(bvh.Build(2,mesh,pred));
 nanort::TriangleIntersector<float> inter(v,f,3*sizeof(float));nanort::Ray<float> r;r.org[0]=.4f;r.org[1]=.6f;r.org[2]=0;r.dir[0]=0;r.dir[1]=0;r.dir[2]=2;r.min_t=0;r.max_t=4;
 nanort::TriangleIntersection<float> h;assert(bvh.Traverse(r,inter,&h));assert(h.prim_id==0&&std::abs(h.t-1)<1e-6f&&std::abs(h.u-.2f)<1e-6f&&std::abs(h.v-.3f)<1e-6f);
 r.org[0]=r.org[1]=1;assert(bvh.Traverse(r,inter,&h));r.max_t=.5f;assert(!bvh.Traverse(r,inter,&h));
 std::puts("NANORT_BASELINE_OK interior_t=1 uv=0.2,0.3 shared_edge=hit finite_segment=miss");}
