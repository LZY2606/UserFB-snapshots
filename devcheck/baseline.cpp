#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <sstream>
#include <iostream>
using namespace bvh::v2;
using V=Vec<double,3>; using T=Tri<double,3>; using N=Node<double,3>; using B=Bvh<N>;
int main() {
  std::vector<T> tris; std::vector<BBox<double,3>> boxes; std::vector<V> centers;
  for(int i=0;i<64;++i) { tris.emplace_back(V(i,0,3),V(i+.8,0,3),V(i,.8,3));
    boxes.push_back(tris.back().get_bbox()); centers.push_back(tris.back().get_center()); }
  DefaultBuilder<N>::Config cfg; cfg.quality=DefaultBuilder<N>::Quality::Low; cfg.max_leaf_size=2;
  auto tree=DefaultBuilder<N>::build(boxes,centers,cfg);
  for(int i=0;i<64;++i) {
    auto dx=V((i*17)%64-i,0,0);
    tris[i].p0=tris[i].p0+dx; tris[i].p1=tris[i].p1+dx; tris[i].p2=tris[i].p2+dx;
  }
  tree.refit([&](N& leaf) {
    auto b=BBox<double,3>::make_empty();
    for(size_t i=leaf.index.first_id();i<leaf.index.first_id()+leaf.index.prim_count();++i)
      b.extend(tris[tree.prim_ids[i]].get_bbox());
    leaf.set_bbox(b);
  });
  auto before=tree.nodes; ReinsertionOptimizer<N>::Config opt; opt.batch_size_ratio=.5;
  ReinsertionOptimizer<N>::optimize(tree,opt); assert(before!=tree.nodes);
  Ray<double,3> ray{V((17*17)%64+.1,.1,0),V(0,0,2),0,10}; size_t hit=999;
  GrowingStack<B::Index> stack;
  tree.intersect<false,true>(ray,tree.get_root().index,stack,[&](size_t a,size_t b) {
    for(size_t i=a;i<b;++i) if(auto h=PrecomputedTri<double>(tris[tree.prim_ids[i]]).intersect(ray)) {
      ray.tmax=std::get<0>(*h); hit=tree.prim_ids[i]; } return hit!=999;
  });
  assert(hit==17 && std::abs(ray.tmax-1.5)<1e-12);
  std::stringstream stream; StdOutputStream out(stream); tree.serialize(out);
  StdInputStream in(stream); auto restored=B::deserialize(in); assert(restored==tree);
  std::cout<<"BVH_REINSERT_REFIT_OK primitive=17 t=1.5 serialized_equal=1\n";
}
