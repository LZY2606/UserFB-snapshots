#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <iostream>
int main() {
  using Mesh = OpenMesh::TriMesh_ArrayKernelT<>;
  Mesh m;
  Mesh::VertexHandle v[5][5];
  for (int y=0;y<5;++y) for(int x=0;x<5;++x)
    v[y][x]=m.add_vertex(Mesh::Point(x,y,0.05f*x*x+0.03f*y*y));
  for (int y=0;y<4;++y) for(int x=0;x<4;++x) {
    m.add_face(v[y][x],v[y][x+1],v[y+1][x+1]);
    m.add_face(v[y][x],v[y+1][x+1],v[y+1][x]);
  }
  OpenMesh::Decimater::DecimaterT<Mesh> d(m);
  OpenMesh::Decimater::ModQuadricT<Mesh>::Handle q;
  d.add(q);
  const bool initialized=d.initialize();
  const auto removed=initialized?d.decimate(1):0;
  std::cout<<"initialized="<<initialized<<" removed="<<removed<<"\n";
  bool endpoint_only=true;
  for (auto it=m.vertices_begin();it!=m.vertices_end();++it) if (!m.status(*it).deleted()) { auto vh=*it;
    auto p=m.point(vh); bool original=false;
    for(int y=0;y<5;++y) for(int x=0;x<5;++x)
      if ((p-Mesh::Point(x,y,0.05f*x*x+0.03f*y*y)).norm()<1e-6) original=true;
    endpoint_only=endpoint_only && original;
  }
  std::cout<<"endpoint_only="<<endpoint_only<<"\n";
  return initialized && removed==1 && endpoint_only ? 0:1;
}
