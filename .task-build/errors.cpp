#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <dirent.h>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void build(fssr::IsoOctree* tree){tree->set_max_level(6);for(int i=1;i<16;++i)for(int j=0;j<32;++j){double a=3.141592653589793*i/16,b=6.283185307179586*j/32;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=s.normal;s.color=math::Vec3f(.2f,.5f,.8f);s.scale=.23f;s.confidence=1.f;tree->insert_sample(s);}tree->limit_octree_level();tree->compute_voxels();}
static std::string readf(const std::string&f){std::ifstream i(f,std::ios::binary);return std::string((std::istreambuf_iterator<char>(i)),{});}
static int countdir(const std::string&d){DIR*dir=opendir(d.c_str());int n=0;while(auto*e=readdir(dir)){std::string s=e->d_name; if(s!="."&&s!="..")n++;}closedir(dir);return n;}
int main(){
  fssr::IsoOctree t; build(&t);
  std::string dir=".task-build/errtmp"; system(("mkdir -p "+dir).c_str());
  std::string target=".task-build/keep.ply";
  { std::ofstream o(target); o<<"OLD-CONTENT"; }
  // Bad budget: target preserved, no temp left
  try { fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,target,dir,10); } catch(std::exception&e){ std::cout<<"badbudget threw\n"; }
  std::cout<<"after badbudget target="<<readf(target)<<" tempdirs="<<countdir(dir)<<"\n";
  // Bad temp dir
  try { fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,target,".task-build/does-not-exist",65536); } catch(std::exception&e){ std::cout<<"badtemp threw\n"; }
  std::cout<<"after badtemp target="<<readf(target)<<"\n";
  // Bad output dir
  try { fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,".task-build/nodir/x.ply",dir,65536); } catch(std::exception&e){ std::cout<<"badout threw\n"; }
  std::cout<<"after badout target="<<readf(target)<<" tempdirs="<<countdir(dir)<<"\n";
  // Retry succeeds and replaces target
  auto st=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,target,dir,65536);
  std::cout<<"retry ok v="<<st.vertex_count<<" target-start="<<readf(target).substr(0,3)<<" tempdirs="<<countdir(dir)<<"\n";
  // Interleaved calls sharing a temp dir
  fssr::IsoOctree t2; build(&t2);
  auto s1=fssr::extract_stream_surface(&t,fssr::INTERPOLATION_CUBIC,".task-build/i1.ply",dir,65536);
  auto s2=fssr::extract_stream_surface(&t2,fssr::INTERPOLATION_LINEAR,".task-build/i2.ply",dir,65536);
  std::cout<<"interleaved ok "<<s1.vertex_count<<","<<s2.vertex_count<<" same-bytes="<<(readf(".task-build/i1.ply")!=readf(".task-build/i2.ply"))<<" tempdirs="<<countdir(dir)<<"\n";
  return 0;
}
