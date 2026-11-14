#include <OpenVolumeMesh/Mesh/TetrahedralGeometryKernel.hh>
#include <OpenVolumeMesh/Geometry/VectorT.hh>
#include <type_traits>
#include <utility>
#include <iostream>
#include <cmath>
using namespace OpenVolumeMesh;
using Mesh=TetrahedralGeometryKernel<Geometry::Vec3d>;
template<class M,class=void> struct has_flip:std::false_type{};
template<class M> struct has_flip<M,std::void_t<decltype(std::declval<M&>().flip23(FaceHandle{},std::declval<std::vector<CellHandle>&>()))>>:std::true_type{};
int main(){
 Mesh m; auto a=m.add_vertex({0,0,0}),b=m.add_vertex({1,0,0}),c=m.add_vertex({0,1,0});
 auto d=m.add_vertex({.25,.25,1}),e=m.add_vertex({.25,.25,-1});
 auto x=m.add_cell(a,b,c,d,true),y=m.add_cell(a,c,b,e,true);
 if(!x.is_valid()||!y.is_valid())return 1;
 unsigned internal=0,boundary=0;for(auto f:m.faces()){if(m.is_boundary(f))++boundary;else ++internal;}
 double v=m.signed_volume(x)+m.signed_volume(y);
 if(m.n_logical_cells()!=2||internal!=1||boundary!=6||std::abs(v-1.0/3)>1e-12)return 2;
 std::cout<<"BASELINE_OK tets=2 internal=1 boundary=6 volume="<<v<<"\nflip23_available="<<has_flip<Mesh>::value<<"\n";
}
