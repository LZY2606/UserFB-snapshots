#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
using namespace OpenSubdiv;
int main() {
  int counts[]={4,4,4,4,4,4};
  int indices[]={0,1,2,3,4,7,6,5,0,4,5,1,1,5,6,2,2,6,7,3,3,7,4,0};
  double xyz[]={-1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,-1,-1,1,1,-1,1,1,1,1,-1,1,1};
  Far::TopologyDescriptor d; d.numVertices=8; d.numFaces=6;
  d.numVertsPerFace=counts; d.vertIndicesPerFace=indices;
  using F=Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
  std::unique_ptr<Far::TopologyRefiner> mesh(F::Create(d,F::Options(Sdc::SCHEME_CATMARK)));
  assert(mesh); Bfr::RefinerSurfaceFactory<> factory(*mesh);
  Bfr::Surface<double> s; assert(factory.InitVertexSurface(0,&s));
  std::vector<double> points(s.GetNumPatchPoints()*3), w(s.GetNumControlPoints());
  s.PreparePatchPoints(xyz,3,points.data(),3);
  double uv[]={.31,.42}, p[3],du[3],dv[3];
  s.Evaluate(uv,points.data(),3,p,du,dv); s.EvaluateStencil(uv,w.data());
  double sum=0,n=0; for(double x:w) sum+=x;
  for(int i=0;i<3;++i) {assert(std::isfinite(p[i])); n+=du[i]*du[i]+dv[i]*dv[i];}
  assert(std::abs(sum-1)<1e-10 && n>0);
  std::cout << "OPENSUBDIV_CUBE_EVALUATION_OK faces=6 stencil_sum=1\n";
}
