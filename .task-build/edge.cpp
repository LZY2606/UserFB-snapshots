#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
int main(int argc,char**argv){
  // all-negative values => no crossing
  fssr::IsoOctree t; t.set_max_level(4);
  for(int i=0;i<8;++i){fssr::Sample s; s.pos=math::Vec3f(i*.1f,0,0); s.normal=math::Vec3f(1,0,0); s.scale=0.3f; s.confidence=1; s.color=math::Vec3f(0,0,0); t.insert_sample(s);} 
  t.limit_octree_level(); t.compute_voxels();
  try { auto st=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,".task-build/empty.ply",".task-build/tt",65536); std::cout<<"empty v="<<st.vertex_count<<" t="<<st.triangle_count<<" peak="<<st.peak_working_bytes<<"\n"; } catch(std::exception&e){ std::cout<<"empty threw: "<<e.what()<<"\n"; }
  // empty octree
  try { fssr::IsoOctree e; auto st=fssr::extract_stream_surface(&e,fssr::INTERPOLATION_CUBIC,".task-build/empty2.ply",".task-build/tt",65536); std::cout<<"emptytree v="<<st.vertex_count<<" t="<<st.triangle_count<<"\n"; } catch(std::exception&e){ std::cout<<"emptytree threw: "<<e.what()<<"\n"; }
  // bad budget
  try { fssr::IsoOctree e; fssr::extract_stream_surface(&e,fssr::INTERPOLATION_CUBIC,".task-build/x.ply",".task-build/tt",100); std::cout<<"badbudget NOT thrown\n"; } catch(std::invalid_argument&e){ std::cout<<"badbudget ok: "<<e.what()<<"\n"; }
  // bad temp
  try { fssr::IsoOctree e; fssr::extract_stream_surface(&e,fssr::INTERPOLATION_CUBIC,".task-build/x.ply",".task-build/nope",65536); std::cout<<"badtemp NOT thrown\n"; } catch(std::runtime_error&e){ std::cout<<"badtemp ok: "<<e.what()<<"\n"; }
  return 0;
}
