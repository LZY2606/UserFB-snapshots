#include <iostream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
int main(){ try { fssr::IsoOctree e; auto st = fssr::extract_stream_surface(&e, fssr::INTERPOLATION_CUBIC, ".task-build/empty2.ply", ".task-build/tt", 65536); std::cout<<"ok v="<<st.vertex_count<<" t="<<st.triangle_count<<"\n"; } catch(std::exception&e){ std::cout<<"threw "<<e.what()<<"\n"; } }
