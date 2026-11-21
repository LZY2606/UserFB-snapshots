#include <iostream>
#include "mve/mesh.h"
#include "mve/mesh_io_ply.h"
int main(int argc,char**argv){ try { auto m=mve::geom::load_ply_mesh(argv[1]); std::cout<<"loaded v="<<m->get_vertices().size()<<" t="<<m->get_faces().size()/3<<" colors="<<m->get_vertex_colors().size()<<" vals="<<m->get_vertex_values().size()<<" conf="<<m->get_vertex_confidences().size()<<"\n"; } catch(std::exception&e){std::cout<<"fail "<<e.what()<<"\n";} }
