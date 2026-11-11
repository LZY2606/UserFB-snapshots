#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/vertex_position_geometry.h"
#include "geometrycentral/surface/heat_method_distance.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace geometrycentral;
using namespace geometrycentral::surface;
int main(int argc, char**) {
  std::vector<std::vector<size_t>> faces={{0,1,2},{0,2,3},{4,5,6},{4,6,7}};
  std::vector<Vector3> pos={{0,0,0},{1,0,0},{1,1,0},{0,1,0},
                           {10,0,0},{30,0,0},{30,20,0},{10,20,0}};
  ManifoldSurfaceMesh mesh(faces); VertexData<Vector3> vp(mesh);
  for(size_t i=0;i<8;++i) vp[mesh.vertex(i)]=pos[i];
  VertexPositionGeometry geom(mesh,vp);
  HeatMethodDistanceSolver solver(geom,1.0,false);
  auto d=solver.computeDistance(mesh.vertex(0));
  int phantom=0; for(size_t i=4;i<8;++i) if(std::isfinite(d[mesh.vertex(i)])) ++phantom;
  for(size_t i=0;i<4;++i) assert(std::isfinite(d[mesh.vertex(i)]));
  std::cout<<"HEAT_UNSOURCED_COMPONENT_FINITE="<<phantom<<"\n";
  ManifoldSurfaceMesh single({{0,1,2},{0,2,3}});
  VertexData<Vector3> sp(single); for(size_t i=0;i<4;++i) sp[single.vertex(i)]=pos[i];
  VertexPositionGeometry sg(single,sp);
  HeatMethodDistanceSolver ss(sg,1.0,false); auto sd=ss.computeDistance(single.vertex(0));
  double delta=std::abs(d[mesh.vertex(2)]-sd[single.vertex(2)]);
  std::cout<<"HEAT_SOURCED_COMPONENT_DELTA="<<delta<<"\n";
  if(argc>1) { if(phantom || delta>1e-8) return 1; return 0; }
  assert(phantom==4 && delta>1e-5);
  std::cout<<"GEOMETRYCENTRAL_DISCONNECTED_REPRO_OK\n";
}
