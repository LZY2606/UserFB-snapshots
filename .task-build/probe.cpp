#include <cmath>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
#include "fssr/hermite.h"
#include "mve/mesh.h"

static void build(fssr::IsoOctree* tree, int max_level, int ni, int nj, float sc) {
    tree->set_max_level(max_level);
    for (int i=1;i<ni;++i) for (int j=0;j<nj;++j) {
        double a=3.141592653589793*i/ni, b=6.283185307179586*j/nj;
        fssr::Sample s;
        s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));
        s.pos=s.normal;
        s.color=math::Vec3f(.2f,.5f,.8f);
        s.scale=(j<nj/2?sc:sc*1.4f);
        s.confidence=1.0f;
        tree->insert_sample(s);
    }
    tree->limit_octree_level();
    tree->compute_voxels();
}
int main(int argc,char**argv){
  int mode = argc>1?atoi(argv[1]):0;
  if(mode==0){
    fssr::IsoOctree tree; build(&tree,6,16,32,0.23f);
    fssr::IsoSurface iso(&tree,fssr::INTERPOLATION_CUBIC);
    auto mesh=iso.extract_mesh();
    std::cout<<"old v="<<mesh->get_vertices().size()<<" t="<<mesh->get_faces().size()/3<<"\n";
  } else {
    fssr::IsoOctree tree; build(&tree,6,16,32,0.23f);
    auto st = fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC, argv[2], argv[3], (size_t)atoi(argv[4]));
    std::cout<<"new v="<<st.vertex_count<<" t="<<st.triangle_count<<" peak="<<st.peak_working_bytes<<" io="<<st.temp_read_bytes<<"/"<<st.temp_write_bytes<<" passes="<<st.merge_passes<<" maxp="<<st.max_polygon_vertices<<" triws="<<st.triangulation_workspace_bytes<<"\n";
  }
}
