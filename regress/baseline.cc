#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace trimesh;
int main(){
 TriMesh::set_verbose(0);TriMesh m;
 for(int y=0;y<5;y++)for(int x=0;x<5;x++){float a=(x-2)*0.3f,b=(y-2)*0.3f;m.vertices.push_back(point(a,b,0.4f*a*a+0.7f*b*b));}
 for(int y=0;y<4;y++)for(int x=0;x<4;x++){int i=5*y+x;m.faces.push_back(TriMesh::Face(i,i+1,i+5));m.faces.push_back(TriMesh::Face(i+1,i+6,i+5));}
 m.need_normals();m.need_curvatures();m.need_dcurv();m.need_pointareas();m.need_bbox();
 assert(m.curv1.size()==25&&m.dcurv.size()==25);float k=m.curv1[12],a=m.pointareas[12];assert(std::fabs(k)>0.1f&&a>0);
 scale(&m,2.f);assert(m.curv1[12]==k&&m.pointareas[12]==a);
 TriMesh fresh;fresh.vertices=m.vertices;fresh.faces=m.faces;fresh.need_curvatures();fresh.need_pointareas();
 assert(std::fabs(fresh.curv1[12]-k*0.5f)<0.02f);assert(std::fabs(fresh.pointareas[12]-4*a)<1e-5f);
 std::printf("STALE_K_RATIO=%.3f FRESH_K_RATIO=%.3f\n",m.curv1[12]/k,fresh.curv1[12]/k);
 std::puts("TRIMESH_DIFFERENTIAL_BASELINE_OK");
}
