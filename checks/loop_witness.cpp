#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace OpenSubdiv;
void require(bool v, const char* msg) { if (!v) throw std::runtime_error(msg); }
int main() {
  try {
    int sizes[]={3,3,3,3}, ids[]={0,2,1,0,1,3,1,2,3,2,0,3};
    double xyz[]={0,0,0, 1.3,.1,.2, -.2,1.1,.4, .3,-.4,1.7};
    Far::TopologyDescriptor d; d.numVertices=4; d.numFaces=4;
    d.numVertsPerFace=sizes; d.vertIndicesPerFace=ids;
    using F=Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    std::unique_ptr<Far::TopologyRefiner> mesh(F::Create(d,F::Options(Sdc::SCHEME_LOOP)));
    require(bool(mesh),"topology creation failed");
    Bfr::RefinerSurfaceFactory<> factory(*mesh);
    Bfr::Surface<double> s; require(factory.InitVertexSurface(0,&s),"surface creation failed");
    require(!s.IsRegular()&&!s.IsLinear(),"expected irregular Loop surface");
    std::vector<double> points(3*s.GetNumPatchPoints());
    s.PreparePatchPoints(xyz,3,points.data(),3);
    for (double scale : {1.0, .5}) {
    double uv[]={.3*scale,.35*scale}, p[3],du[3],dv[3],duu[3],duv[3],dvv[3];
    s.Evaluate(uv,points.data(),3,p,du,dv,duu,duv,dvv);
    double previous=0;
    std::cout<<std::setprecision(12);
    for(double step:{1e-3,5e-4}) {
      double h=step*scale;
      double pp[3],pm[3],mp[3],mm[3];
      auto pos=[&](double u,double v,double* out) {
        double q[]={u,v}; s.Evaluate(q,points.data(),3,out);
      };
      pos(uv[0]+h,uv[1]+h,pp);pos(uv[0]+h,uv[1]-h,pm);
      pos(uv[0]-h,uv[1]+h,mp);pos(uv[0]-h,uv[1]-h,mm);
      double fd=(pp[2]-pm[2]-mp[2]+mm[2])/(4*h*h);
      require(std::isfinite(fd)&&std::isfinite(duv[2]),"nonfinite derivative");
      require(std::abs(fd)>.01,"uninformative mixed derivative");
      require(fd*duv[2]<0&&std::abs(fd+duv[2])<1e-5,"known sign defect not reproduced");
      if(previous) require(std::abs(previous-fd)<1e-5,"position difference unstable");
      previous=fd;
      std::cout<<"BFR_LOOP_OBSERVATION face=0 u="<<uv[0]<<" v="<<uv[1]<<" component=z h="<<h
               <<" analytic="<<duv[2]<<" position_fd="<<fd<<"\n";
    }
    }
    std::cout<<"OPENSUBDIV_BFR_LOOP_DEFECT_OBSERVED samples=2 steps=4\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
