#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <iostream>
#include <memory>
using namespace trimesh;
static TriMesh* cap(bool lower, bool dense) {
 TriMesh* m=new TriMesh;
 if(!lower) {
  m->vertices={point(0,0,0),point(1,0,0),point(1,1,0),point(0,1,0),point(.5,.5,1)};
 } else if(dense) {
  m->vertices={point(0,0,0),point(0,1,0),point(1,1,0),point(1,0,0),point(.5,0,0),point(.5,.5,-1)};
 } else {
  m->vertices={point(0,0,0),point(0,1,0),point(1,1,0),point(1,0,0),point(.5,.5,-1)};
 }
 int ring=int(m->vertices.size())-1;
 for(int i=0;i<ring;++i)m->faces.push_back(TriMesh::Face(i,(i+1)%ring,ring));
 return m;
}
static int boundary_edges(TriMesh* m) {
 m->need_across_edge(); int n=0;
 for(size_t i=0;i<m->across_edge.size();++i)
  for(int k=0;k<3;++k) if(m->across_edge[i][k]<0) ++n;
 return n;
}
int main() {
 TriMesh::set_verbose(0);
 std::unique_ptr<TriMesh> upper(cap(false,false)),lower(cap(true,false)),dense(cap(true,true));
 std::vector<TriMesh*> pair={upper.get(),lower.get()};
 std::unique_ptr<TriMesh> equal(join(pair,1e-5f));remove_unused_vertices(equal.get());
 if(equal->vertices.size()!=6 || equal->faces.size()!=8 || boundary_edges(equal.get())!=0)return 1;
 pair[1]=dense.get();std::unique_ptr<TriMesh> unequal(join(pair,1e-5f));remove_unused_vertices(unequal.get());
 if(unequal->vertices.size()!=7 || unequal->faces.size()!=9 || boundary_edges(unequal.get())!=3)return 2;
 pair[1]=lower.get();std::unique_ptr<TriMesh> separate(join(pair));shared(separate.get(),1e-5f);
 if(separate->vertices.size()!=6 || boundary_edges(separate.get())!=0)return 3;
 std::cout<<"BASELINE join-equal closed=1 join-unequal boundary=3 shared-equal closed=1 PASS\n";
 return 0;
}
