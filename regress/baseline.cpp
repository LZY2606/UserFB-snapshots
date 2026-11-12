#include "include/MC33.h"
#include <cmath>
#include <cassert>
#include <cstdio>
int main(){
 grid3d g;assert(g.set_grid_dimensions(3,3,3)==0);
 for(unsigned x=0;x<3;x++)for(unsigned y=0;y<3;y++)for(unsigned z=0;z<3;z++)g.set_grid_value(x,y,z,x+2*y+3*z);
 MC33 mc;mc.set_grid3d(g);surface s;assert(mc.calculate_isosurface(s,2.5)==0);
 assert(s.get_num_vertices()>0 && s.get_num_triangles()>0);
 for(unsigned i=0;i<s.get_num_vertices();i++){auto v=s.getVertex(i);assert(std::fabs(v[0]+2*v[1]+3*v[2]-2.5)<1e-5);}
 unsigned nv=0,nt=0;assert(mc.size_of_isosurface(2.5,nv,nt)>0);assert(nv==s.get_num_vertices() && nt==s.get_num_triangles());
 std::printf("MC33_PLANE_COUNT_OK vertices=%u triangles=%u\n",nv,nt);
}
