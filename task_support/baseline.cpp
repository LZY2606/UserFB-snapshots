#define main nanort_original_curves_main
#include "examples/curves_primitive/main.cc"
#undef main
#include <iomanip>
#include <iostream>
#include <cstdlib>

int main() {
  // Public starting geometry: unchanged upstream line approximation.
  const float p[] = {-1,0,10, -1.0f/3,0,10, 1.0f/3,0,10, 1,0,10};
  const float flat[] = {.2f,.2f,.2f,.2f};
  const float bulge[] = {.2f,2,2,.2f};
  nanort::Ray<float> ray;
  ray.org[0]=0; ray.org[1]=0; ray.org[2]=0;
  ray.dir[0]=0; ray.dir[1]=0; ray.dir[2]=1;
  ray.min_t=0; ray.max_t=100;
  float ts[2]={}; bool side[2]={};
  for(int i=0;i<2;++i) {
    const float* r=i?bulge:flat;
    CurveGeometry geom(p,r); CurvePred pred(p);
    nanort::BVHAccel<float> bvh;
    if(!bvh.Build(1,geom,pred)) return 2;
    CurveIntersector<CurveIntersection> inter(p,r);
    CurveIntersection h;
    ray.org[1]=0;
    if(!bvh.Traverse(ray,inter,&h)||h.prim_id!=0) return 3;
    ts[i]=h.t;
    ray.org[1]=.5f;
    side[i]=bvh.Traverse(ray,inter,&h);
  }
  if(ts[0]!=10||ts[1]!=10||side[0]||side[1]) return 4;
  std::cout << "CURVE_START center_t=10 interior_radius_ignored=1 side_miss=1\n";
  std::cout << "CURVE_BASELINE_COMPLETE\n";
}
