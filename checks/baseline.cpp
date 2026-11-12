#include "nanort.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
  const double v[] = {-1,-1,0, 1,-1,0, 1,1,2, -1,1,0};
  const unsigned f[] = {0,1,2, 0,2,3};
  nanort::TriangleMesh<double> mesh(v,f,3*sizeof(double));
  nanort::TriangleSAHPred<double> pred(v,f,3*sizeof(double));
  nanort::BVHAccel<double> bvh;
  assert(bvh.Build(2,mesh,pred));
  nanort::Ray<double> ray;
  ray.org[0]=0; ray.org[1]=0; ray.org[2]=3;
  ray.dir[0]=0; ray.dir[1]=0; ray.dir[2]=-1;
  ray.max_t=10;
  nanort::TriangleIntersector<double> inter(v,f,3*sizeof(double));
  nanort::TriangleIntersection<double> hit;
  assert(bvh.Traverse(ray,inter,&hit));
  assert(std::abs(hit.t-2)<1e-12);
  ray.org[0]=4;
  assert(!bvh.Traverse(ray,inter,&hit));
  std::cout << "TRIANGLE_BASELINE_OK center_t=2 bilinear_target_t=2.5\n";
}
