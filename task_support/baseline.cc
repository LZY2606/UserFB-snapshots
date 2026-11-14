#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cmath>
#include <iostream>
using namespace trimesh;
#define CHECK(x) do {if(!(x)){std::cerr<<"BASELINE failure "<<__LINE__<<": "<<#x<<"\n";return 1;}}while(0)
int main(){
 TriMesh::set_verbose(0);TriMesh m;
 m.vertices={point(0,0,0),point(2,0,0),point(0,1,0),point(.3f,.4f,1.5f)};
 m.faces={TriMesh::Face(0,2,1),TriMesh::Face(0,1,3),TriMesh::Face(1,2,3),TriMesh::Face(2,0,3)};
 m.colors.assign(4,Color(.2f,.3f,.4f));m.confidences.assign(4,.7f);
 m.need_neighbors();m.need_across_edge();m.need_normals();m.need_bbox();
 CHECK(m.neighbors.size()==4 && m.neighbors[0].size()==3);
 for(size_t f=0;f<m.faces.size();++f)for(int k=0;k<3;++k)CHECK(m.across_edge[f][k]>=0);
 std::vector<point> rest=m.vertices;std::vector<vec> normals=m.normals;
 xform r=xform::rot(.4,vec(1,2,3));xform xf=xform::trans(2,-1,.5)*r*xform::scale(1.7);
 apply_xform(&m,xf);
 for(size_t i=0;i<4;++i){CHECK(len(m.vertices[i]-xf*rest[i])<2e-6);CHECK(len(m.normals[i]-norm_xf(r)*normals[i])<2e-5);}
 CHECK(m.bbox.valid);CHECK(m.faces.size()==4 && m.colors.size()==4 && m.confidences[0]==.7f);
 m.need_pointareas();double sum=0,area=0;
 for(size_t i=0;i<m.pointareas.size();++i)sum+=m.pointareas[i];
 for(size_t f=0;f<m.faces.size();++f){TriMesh::Face a=m.faces[f];area+=.5*len((m.vertices[a[1]]-m.vertices[a[0]]) CROSS (m.vertices[a[2]]-m.vertices[a[0]]));}
 CHECK(std::fabs(sum-area)<1e-5*area);
 std::cout<<"BASELINE rigid-scale positions normals adjacency areas PASS\n";return 0;
}
