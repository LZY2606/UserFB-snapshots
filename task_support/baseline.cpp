#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>
#include <iostream>
#include <string>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
using Mesh=OpenMesh::TriMesh_ArrayKernelT<>;
int main(){try{
 Mesh m;OpenMesh::VPropHandleT<std::string> tag;m.add_property(tag,"tag");
 const int w=9;for(int y=0;y<w;++y)for(int x=0;x<w;++x){auto v=m.add_vertex(Mesh::Point(x,y,.013*x*x+.021*y*y+.007*x*y));m.property(tag,v)="vertex_"+std::to_string(v.idx());}
 for(int y=0;y<w-1;++y)for(int x=0;x<w-1;++x){int a=y*w+x;CHECK(m.add_face(Mesh::VertexHandle(a),Mesh::VertexHandle(a+1),Mesh::VertexHandle(a+w+1)).is_valid());CHECK(m.add_face(Mesh::VertexHandle(a),Mesh::VertexHandle(a+w+1),Mesh::VertexHandle(a+w)).is_valid());}
 m.request_vertex_status();m.request_edge_status();m.request_face_status();m.request_face_normals();m.update_face_normals();
 for(auto v=m.vertices_begin();v!=m.vertices_end();++v)if(m.is_boundary(*v))m.status(*v).set_locked(true);
 using Dec=OpenMesh::Decimater::DecimaterT<Mesh>;Dec d(m);OpenMesh::Decimater::ModQuadricT<Mesh>::Handle q;CHECK(d.add(q));CHECK(d.initialize());
 auto removed=d.decimate(3);CHECK(removed==3);size_t live=0;for(auto v=m.vertices_sbegin();v!=m.vertices_end();++v){++live;CHECK(m.property(tag,*v)=="vertex_"+std::to_string(v->idx()));}
 CHECK(live==78);CHECK(m.n_vertices()==81);CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
 std::cout<<"BASELINE quadric collapses=3 live_vertices=78 allocated_vertices=81 checker=ok\n";
 m.garbage_collection();CHECK(m.n_vertices()==78);CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
 std::cout<<"BASELINE property-string retained and garbage-collection compacted PASS\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
