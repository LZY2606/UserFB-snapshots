#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cmath>
using namespace bvh::v2;
using N=Node<float,3>; using V=Vec<float,3>; using T=Tri<float,3>; using B=Bvh<N>;
static size_t hit(const B& b,const std::vector<T>& ts,float &distance) {
    Ray<float,3> ray{V(.2f,.2f,0),V(0,0,1),0,100};
    size_t id=size_t(-1); GrowingStack<N::Index> stack;
    b.intersect<false,true>(ray,b.get_root().index,stack,[&](size_t begin,size_t end){
        for(size_t i=begin;i<end;++i) if(auto h=PrecomputedTri<float>(ts[b.prim_ids[i]]).intersect(ray)) {
            id=b.prim_ids[i];ray.tmax=std::get<0>(*h);
        }
        return id!=size_t(-1);
    });
    distance=ray.tmax;return id;
}
int main(int argc,char**argv){
    std::vector<T> ts; std::vector<BBox<float,3>> boxes;std::vector<V> centers;
    for(int i=0;i<16;++i){float x=float((i*7)%16)*3;ts.emplace_back(V(x,0,2),V(x+1,0,2),V(x,1,2));}
    for(auto&t:ts){boxes.push_back(t.get_bbox());centers.push_back(t.get_center());}
    DefaultBuilder<N>::Config cfg;cfg.quality=DefaultBuilder<N>::Quality::Low;cfg.max_leaf_size=1;
    auto b=DefaultBuilder<N>::build(boxes,centers,cfg);
    float d=0;if(hit(b,ts,d)!=0 || std::abs(d-2)>1e-5)return 1;
    size_t leaves=0;for(auto&n:b.nodes)if(n.is_leaf())++leaves;
    std::cout<<"BINARY_BASELINE nodes="<<b.nodes.size()<<" leaves="<<leaves<<" closest=0 t="<<d<<"\n";
    std::ostringstream os(std::ios::binary);StdOutputStream out(os);b.serialize(out);
    if(argc==2){std::ofstream f(argv[1],std::ios::binary);f<<os.str();if(!f)return 2;}
    std::istringstream is(os.str(),std::ios::binary);StdInputStream in(is);auto copy=B::deserialize(in);
    if(copy!=b)return 3;
    for(auto&t:ts){t.p0[2]+=1;t.p1[2]+=1;t.p2[2]+=1;}
    b.refit([&](N&n){auto box=BBox<float,3>::make_empty();for(size_t i=n.index.first_id();i<n.index.first_id()+n.index.prim_count();++i)box.extend(ts[b.prim_ids[i]].get_bbox());n.set_bbox(box);});
    if(hit(b,ts,d)!=0 || std::abs(d-3)>1e-5)return 4;
    auto sub=b.extract_bvh(b.nodes[0].index.first_id());
    if(sub.prim_ids.empty() || sub.prim_ids.size()>=ts.size())return 5;
    std::cout<<"BINARY_REFIT t="<<d<<" subtree_primitives="<<sub.prim_ids.size()<<" stream_bytes="<<os.str().size()<<"\nBASELINE_OK\n";
}
