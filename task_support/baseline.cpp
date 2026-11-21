#include <cmath>
#include <iostream>
#include <stdexcept>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
int main() {
    fssr::IsoOctree tree;
    tree.set_max_level(6);
    for (int i=1;i<16;++i) for (int j=0;j<32;++j) {
        double a=3.141592653589793*i/16, b=6.283185307179586*j/32;
        fssr::Sample s;
        s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));
        s.pos=s.normal;
        s.color=math::Vec3f(.2f,.5f,.8f);
        s.scale=(j<16?.23f:.33f);
        s.confidence=1.0f;
        tree.insert_sample(s);
    }
    tree.limit_octree_level();
    tree.compute_voxels();
    fssr::IsoSurface iso(&tree,fssr::INTERPOLATION_LINEAR);
    auto mesh=iso.extract_mesh();
    auto const& v=mesh->get_vertices(); auto const& f=mesh->get_faces();
    if(v.empty()||f.empty()||f.size()%3) throw std::runtime_error("empty surface");
    if(mesh->get_vertex_colors().size()!=v.size()||mesh->get_vertex_values().size()!=v.size()||mesh->get_vertex_confidences().size()!=v.size()) throw std::runtime_error("attributes");
    for(auto x:f) if(x>=v.size()) throw std::runtime_error("index");
    for(auto const& x:v) for(int d=0;d<3;++d) if(!std::isfinite(x[d])) throw std::runtime_error("position");
    std::cout<<"FSSR_BASELINE vertices="<<v.size()<<" triangles="<<f.size()/3<<" levels="<<tree.get_num_levels()<<" attributes=complete indices=valid\n";
}
