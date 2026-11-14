#include <cinolib/meshes/hexmesh.h>
#include <cinolib/subdivision_legacy_hexa_schemes.h>
#include <cinolib/subdivision_midpoint.h>
#include <iostream>
#include <cmath>
using namespace cinolib;
static Hexmesh<> pair_mesh() {
    std::vector<vec3d> v;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<3;++x)v.emplace_back(x,y,z);
    std::vector<std::vector<uint>> h{{0,1,4,3,6,7,10,9},{1,2,5,4,7,8,11,10}};
    return Hexmesh<>(v,h);
}
int main(){
    auto m=pair_mesh();
    if(m.num_polys()!=2||m.num_faces()!=11)return 2;
    uint edge=m.num_edges();
    for(uint i=0;i<m.num_edges();++i){auto v=m.edge_vert_ids(i);if((v[0]==0&&v[1]==3)||(v[0]==3&&v[1]==0))edge=i;}
    if(edge==m.num_edges())return 3;
    auto sheet=m.poly_sheet(edge);if(sheet.size()!=2)return 4;
    std::cout<<"SHEET cells="<<sheet.size()<<"\n";
    m.poly_data(0).label=7;m.poly_data(1).label=9;
    auto uniform=m;uniform.poly_subdivide(hex_to_grid_2x2x2);
    if(uniform.num_polys()!=16||uniform.num_verts()!=45)return 5;
    std::cout<<"UNIFORM cells="<<uniform.num_polys()<<" verts="<<uniform.num_verts()<<" first_label="<<uniform.poly_data(0).label<<"\n";
    Hexmesh<> midpoint;subdivision_midpoint(m,midpoint);
    if(midpoint.num_polys()!=16||midpoint.num_verts()!=45)return 6;
    uint same=0,total=0;
    for(uint f=0;f<midpoint.num_faces();++f)if(midpoint.adj_f2p(f).size()==2){const auto &p=midpoint.adj_f2p(f);++total;if(midpoint.poly_face_is_CCW(p[0],f)==midpoint.poly_face_is_CCW(p[1],f))++same;}
    std::cout<<"MIDPOINT cells="<<midpoint.num_polys()<<" interior="<<total<<" same_winding="<<same<<"\nBASELINE_OK\n";
}
