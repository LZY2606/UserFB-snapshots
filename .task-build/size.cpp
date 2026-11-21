#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <iostream>
#include <iterator>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void build(fssr::IsoOctree* tree,int lvl,int ni,int nj){tree->set_max_level(lvl);for(int i=1;i<ni;++i)for(int j=0;j<nj;++j){double a=3.141592653589793*i/ni,b=6.283185307179586*j/nj;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=s.normal;s.color=math::Vec3f(.2f,.5f,.8f);s.scale=.23f;s.confidence=1.f;tree->insert_sample(s);}tree->limit_octree_level();tree->compute_voxels();}
int main(int argc,char**argv){int lvl=atoi(argv[1]),ni=atoi(argv[2]),nj=atoi(argv[3]);size_t b=atol(argv[4]);fssr::IsoOctree t;build(&t,lvl,ni,nj);auto st=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,argv[5],argv[6],b);std::cout<<"v="<<st.vertex_count<<" t="<<st.triangle_count<<" peak="<<st.peak_working_bytes<<" passes="<<st.merge_passes<<" io="<<st.temp_read_bytes<<"/"<<st.temp_write_bytes<<" ws="<<st.triangulation_workspace_bytes<<"\n";}
