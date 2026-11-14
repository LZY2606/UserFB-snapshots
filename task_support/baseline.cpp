#include "pmp/algorithms/remeshing.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace pmp;
SurfaceMesh grid() {
 SurfaceMesh m; Vertex v[5][5];
 for(int j=0;j<5;j++)for(int i=0;i<5;i++)v[j][i]=m.add_vertex(Point(i*.5,j*.5,0));
 for(int j=0;j<4;j++)for(int i=0;i<4;i++){
 m.add_triangle(v[j][i],v[j][i+1],v[j+1][i+1]);
 m.add_triangle(v[j][i],v[j+1][i+1],v[j+1][i]); }
 return m;
}
int main(){
 auto a=grid(),b=grid();
 auto ma=a.add_vertex_property<Matrix<Scalar,3,3>>("v:metric");
 auto mb=b.add_vertex_property<Matrix<Scalar,3,3>>("v:metric");
 for(auto v:a.vertices()){ma[v]=Matrix<Scalar,3,3>(0);mb[v]=Matrix<Scalar,3,3>(0);
 ma[v](0,0)=100;ma[v](1,1)=1;ma[v](2,2)=1;
 mb[v](0,0)=1;mb[v](1,1)=100;mb[v](2,2)=1;}
 uniform_remeshing(a,.3,2,true);uniform_remeshing(b,.3,2,true);
 assert(a.n_vertices()==b.n_vertices());assert(a.n_faces()==b.n_faces());
 double delta=0;for(auto v:a.vertices())delta+=norm(a.position(v)-b.position(v));
 assert(delta==0);assert(a.n_faces()>32);
 std::cout<<"BASELINE_SCALAR_REMESH vertices="<<a.n_vertices()<<" faces="<<a.n_faces()<<" tensor_swap_delta="<<delta<<"\n";
 std::cout<<"BASELINE_OK existing_uniform_ignores_tensor\n";
}
