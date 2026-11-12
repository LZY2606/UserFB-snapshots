#include <FastBVH.h>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace FastBVH;
using V=Vector3<double>;
struct P {BBox<double> box;int id;};
struct Box {BBox<double> operator()(const P& p)const{return p.box;}};
struct Inter {int* calls;Intersection<double,P> operator()(const P&p,const Ray<double>&r)const{
 ++*calls;double a,b; if(!p.box.intersect(r,&a,&b)||b<0)return {};return {std::max(0.,a),&p};}};
int main(){std::vector<P> p;for(int i=0;i<64;++i)p.push_back({{{double(i*4),-1,4},{double(i*4+1),1,5}},i});
 auto bvh=DefaultBuilder<double>{}(p,Box{});int calls=0;Traverser<double,P,Inter> t(bvh,Inter{&calls});
 int count=0;for(int i=0;i<6;++i){Ray<double> r({.25+i*8.,.1,0},{.001,.002,1});auto hit=t.traverse(r);assert(hit);assert(hit.object->id==i*2);assert(std::abs(hit.t-4)<1e-12);++count;}
 assert(calls<6*64);Ray<double> miss({-20,20,0},{.01,.01,1});assert(!t.traverse(miss));
 Traverser<double,P,Inter,OnlyTestOcclusion> any(bvh,Inter{&calls});assert(any.traverse(Ray<double>({.2,0,0},{.001,.001,1})));
 std::printf("FASTBVH_BASELINE_OK rays=%d nodes=%zu primitive_calls=%d\n",count,bvh.getNodes().size(),calls);
}
