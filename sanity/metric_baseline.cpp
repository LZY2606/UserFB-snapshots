#include "pmp/surface_mesh.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace pmp;
static void require(bool ok) { if(!ok) throw std::runtime_error("flip baseline mismatch"); }
int main() {
 SurfaceMesh m;
 auto a=m.add_vertex(Point(-1,0,0)), b=m.add_vertex(Point(0,1,0));
 auto c=m.add_vertex(Point(1,0,0)), d=m.add_vertex(Point(0,0,1));
 m.add_triangle(a,b,c); m.add_triangle(a,c,d);
 auto tag=m.add_vertex_property<int>("v:tag",7);
 auto e=m.find_edge(a,c); require(m.is_flip_ok(e));
 m.flip(e); require(m.find_edge(b,d)==e);
 require(m.n_vertices()==4 && m.n_faces()==2 && m.n_edges()==5);
 for(auto v:m.vertices())require(tag[v]==7);
 // Unfolding gives a planar square with old/new diagonals both 2.
 const double chord=norm(m.position(b)-m.position(d));
 require(std::abs(chord-std::sqrt(2.0))<1e-12);
 double copied_integral=0;
 for(auto f:m.faces()) {
  auto h=m.halfedge(f); auto v0=m.to_vertex(h); auto v1=m.to_vertex(m.next_halfedge(h));
  auto v2=m.to_vertex(m.prev_halfedge(h));
  // New intrinsic faces have area 1. Source hat at b had integral 1/3,
  // but retaining the same nodal values on the new diagonal gives 2/3.
  copied_integral += ((v0==b)+(v1==b)+(v2==b))/3.0;
 }
 require(std::abs(copied_integral-2.0/3.0)<1e-12);
 std::cout<<"PMP_FLIP_BASELINE vertices=4 faces=2 chord=sqrt2 intrinsic=2 copied_integral=2/3 source_integral=1/3\n";
}
