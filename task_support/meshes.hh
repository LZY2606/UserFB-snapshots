#pragma once
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
namespace task_fixture {
struct Traits:OpenMesh::DefaultTraits { typedef OpenMesh::Vec3d Point; typedef OpenMesh::Vec3d Normal; };
using Mesh=OpenMesh::TriMesh_ArrayKernelT<Traits>;
using V=Mesh::Point;
Mesh make_mesh(unsigned seed) {Mesh m;Mesh::VertexHandle v[7][7];unsigned s=seed;
  for(int y=0;y<7;++y)for(int x=0;x<7;++x){s=1664525u*s+1013904223u;double z=(double(s>>8)/16777216.-.5)*3.;v[y][x]=m.add_vertex(V(x,y,z));}
  for(int y=0;y<6;++y)for(int x=0;x<6;++x){m.add_face(v[y][x],v[y][x+1],v[y+1][x+1]);m.add_face(v[y][x],v[y+1][x+1],v[y+1][x]);}
  m.request_vertex_status();m.request_edge_status();m.request_face_status();m.request_face_normals();m.update_face_normals();return m;}
}
