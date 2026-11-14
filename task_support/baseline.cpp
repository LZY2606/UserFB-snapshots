#include "nanort.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
template<class T> void check(const char *name) {
    struct Vertex { T x,y,z,padding; } v[]={{-1,-1,2,777},{1,-1,2,777},{0,1,2,777},{-1,-1,4,777},{1,-1,4,777},{0,1,4,777}};
    unsigned faces[]={0,1,2,3,4,5};
    nanort::TriangleMesh<T> mesh(&v[0].x,faces,sizeof(Vertex));
    nanort::TriangleSAHPred<T> pred(&v[0].x,faces,sizeof(Vertex));
    nanort::BVHAccel<T> tree;
    assert(tree.Build(2,mesh,pred));
    nanort::TriangleIntersector<T> isector(&v[0].x,faces,sizeof(Vertex));
    nanort::Ray<T> ray;ray.dir[2]=1;ray.max_t=10;
    nanort::TriangleIntersection<T> hit;
    assert(tree.Traverse(ray,isector,&hit));
    assert(hit.prim_id==0 && std::fabs(hit.t-2)<T(1e-6));
    assert(std::fabs(hit.u-T(.25))<T(1e-6) && std::fabs(hit.v-T(.5))<T(1e-6));
    nanort::BVHTraceOptions opts;opts.skip_prim_id=0;
    assert(tree.Traverse(ray,isector,&hit,opts) && hit.prim_id==1 && hit.t==4);
    ray.max_t=3;assert(!tree.Traverse(ray,isector,&hit,opts));ray.max_t=10;
    // axis is unused for leaf traversal; initialize it for deterministic raw legacy files.
    std::vector<nanort::BVHNode<T> > &nodes=const_cast<std::vector<nanort::BVHNode<T> >&>(tree.GetNodes());
    for (size_t i=0;i<nodes.size();++i) if(nodes[i].flag==1) nodes[i].axis=0;
    std::string path=std::string(".task-build/legacy-")+name+".bvh";
    assert(tree.Dump(path.c_str()));
    nanort::BVHAccel<T> loaded;assert(loaded.Load(path.c_str()));
    assert(loaded.Traverse(ray,isector,&hit) && hit.prim_id==0 && hit.t==2);
    std::cout << "STATIC_BASELINE_OK precision=" << name << " t=2 uv=0.25,0.5 stride=" << sizeof(Vertex) << " serialized=1\n";
}
int main(){check<float>("float");check<double>("double");}
