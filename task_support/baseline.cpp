#include "nanort.h"
#include <iostream>
#include <stdexcept>
int main(){float v[]={-1,-1,1,1,-1,1,0,1,1,-1,-1,2,1,-1,2,0,1,2};unsigned f[]={0,1,2,3,4,5};nanort::TriangleMesh<float> mesh(v,f,12);nanort::TriangleSAHPred<float> pred(v,f,12);nanort::BVHAccel<float> b;nanort::BVHBuildOptions<float> options;if(!b.Build(2,mesh,pred,options))return 1;nanort::Ray<float> ray;ray.org[0]=ray.org[1]=ray.org[2]=0;ray.dir[0]=ray.dir[1]=0;ray.dir[2]=1;ray.min_t=0;ray.max_t=10;nanort::TriangleIntersector<float> inter(v,f,12);nanort::TriangleIntersection<float> h;if(!b.Traverse(ray,inter,&h)||h.prim_id!=0||std::abs(h.t-1)>1e-6)throw std::runtime_error("trace");std::cout<<"nearest opaque primitive="<<h.prim_id<<" t="<<h.t<<" uv="<<h.u<<","<<h.v<<"\n";}
