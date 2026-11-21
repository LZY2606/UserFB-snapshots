#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <fstream>
#include <iostream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void build(fssr::IsoOctree* tree,int lvl,int ni,int nj,float sc,int spheres){tree->set_max_level(lvl);
 for(int s=0;s<spheres;++s){ math::Vec3f c((s%3-1)*1.1f, ((s/3)%3-1)*1.1f, ((s/9)%3-1)*1.1f);
  for(int i=1;i<ni;++i)for(int j=0;j<nj;++j){double a=3.141592653589793*i/ni,b=6.283185307179586*j/nj;fssr::Sample sm;sm.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));sm.pos=c+sm.normal;sm.color=math::Vec3f(.2f,.5f,.8f);sm.scale=sc;sm.confidence=1.f;tree->insert_sample(sm);}}
 tree->limit_octree_level();tree->compute_voxels();}
int main(int argc,char**argv){int lvl=atoi(argv[1]),ni=atoi(argv[2]),nj=atoi(argv[3]),sp=atoi(argv[4]);float sc=atof(argv[5]);size_t b=atol(argv[6]);fssr::IsoOctree t;build(&t,lvl,ni,nj,sc,sp);auto st=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,argv[7],argv[8],b);std::cout<<"v="<<st.vertex_count<<" t="<<st.triangle_count<<" peak="<<st.peak_working_bytes<<" passes="<<st.merge_passes<<" io="<<st.temp_read_bytes<<"/"<<st.temp_write_bytes<<" ws="<<st.triangulation_workspace_bytes<<"\n";}
