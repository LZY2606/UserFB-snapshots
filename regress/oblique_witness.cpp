#include "include/MC33.h"
#include <cmath>
#include <cstdio>
#include <cstring>
// Small starting-point observation, not the requested regression suite.
int main(int argc, char **argv) {
  grid3d g;
  if(g.set_grid_dimensions(3,3,3)) return 2;
  g.set_ratio_aspect(2,3,5); g.set_r0(1,-2,3); g.set_Ang(80,100,70);
  for(unsigned x=0;x<3;x++) for(unsigned y=0;y<3;y++) for(unsigned z=0;z<3;z++)
    g.set_grid_value(x,y,z,x+2*y+3*z);
  const double pi=std::acos(-1.), ca=std::cos(80*pi/180), cb=std::cos(100*pi/180),
    cg=std::cos(70*pi/180), sg=std::sin(70*pi/180);
  // Basis derived from axis lengths and pairwise dot products, not library matrices.
  const double ax=2, bx=3*cg, by=3*sg, cx=5*cb, cy=5*(ca-cb*cg)/sg,
    cz=5*std::sqrt(1-cb*cb-std::pow((ca-cb*cg)/sg,2));
  double nx=1/ax, ny=(2-bx*nx)/by, nz=(3-cx*nx-cy*ny)/cz;
  double len=std::sqrt(nx*nx+ny*ny+nz*nz); nx/=-len;ny/=-len;nz/=-len;
  MC33 mc; if(mc.set_grid3d(g)) return 3;
  surface s; int rc=mc.calculate_isosurface(s,2.5);
  std::printf("GENERATED rc=%d vertices=%u triangles=%u\n",rc,s.get_num_vertices(),s.get_num_triangles());
  if(rc || !s.get_num_vertices() || !s.get_num_triangles())return 4;
  double maxerr=0,maxres=0;
  for(unsigned i=0;i<s.get_num_vertices();i++) {
    const auto n=s.getNormal(i); const auto v=s.getVertex(i);
    double err=std::sqrt(std::pow(n[0]-nx,2)+std::pow(n[1]-ny,2)+std::pow(n[2]-nz,2));
    maxerr=std::fmax(maxerr,err);
    double z=(v[2]-3)/cz,y=(v[1]+2-cy*z)/by,x=(v[0]-1-bx*y-cx*z)/ax;
    maxres=std::fmax(maxres,std::fabs(x+2*y+3*z-2.5));
    if(i==0)std::printf("normal_actual=(%.9g,%.9g,%.9g) expected=(%.9g,%.9g,%.9g)\n",n[0],n[1],n[2],nx,ny,nz);
  }
  if(g.set_interpolation(1))return 5;
  const double ix=.4,iy=.7,iz=.3;
  double observed=g.interpolated_value(1+ax*ix+bx*iy+cx*iz,-2+by*iy+cy*iz,3+cz*iz);
  const double expected=ix+2*iy+3*iz;
  std::printf("MC33_OBLIQUE_OBSERVATION position_residual=%.9g normal_error=%.9g interpolation_actual=%.9g expected=%.9g\n",maxres,maxerr,observed,expected);
  if(maxres>1e-5 || !std::isfinite(observed))return 6;
  bool defect=maxerr>1e-4 && std::fabs(observed-expected)>1e-4;
  bool require_defect=argc==2 && !std::strcmp(argv[1],"--expect-defect");
  if(require_defect) {
    if(!defect)return 7;
    std::puts("MC33_KNOWN_OBLIQUE_DEFECT"); return 0;
  }
  return maxerr>1e-4 || std::fabs(observed-expected)>1e-4 ? 41 : 0;
}
