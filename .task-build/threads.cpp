#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <iterator>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void build(fssr::IsoOctree* tree,int mode){tree->set_max_level(6);for(int i=1;i<16;++i)for(int j=0;j<32;++j){double a=3.141592653589793*i/16,b=6.283185307179586*j/32;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=s.normal;s.color=math::Vec3f(.2f*mode,.5f,.8f);s.scale=.23f;s.confidence=1.f;tree->insert_sample(s);}tree->limit_octree_level();tree->compute_voxels();}
static std::string readf(const std::string&f){std::ifstream i(f,std::ios::binary);return std::string((std::istreambuf_iterator<char>(i)),{});}
int main(){
  std::string dir=".task-build/thtmp"; system(("mkdir -p "+dir).c_str());
  std::vector<fssr::IsoOctree*> trees(4); for(int i=0;i<4;++i){trees[i]=new fssr::IsoOctree(); build(trees[i],i);}
  std::vector<fssr::StreamSurfaceStats> stats(4); std::vector<std::thread> th;
  for(int i=0;i<4;++i) th.emplace_back([&,i]{ stats[i]=fssr::extract_stream_surface(trees[i],(fssr::InterpolationType)i,".task-build/th"+std::to_string(i)+".ply",dir,65536); });
  for(auto&x:th)x.join();
  std::string b0=readf(".task-build/th0.ply"); bool distinct=true;
  for(int i=1;i<4;++i) if(readf(".task-build/th"+std::to_string(i)+".ply")==b0) distinct=false;
  std::cout<<"threads ok v="<<stats[0].vertex_count<<" all-distinct="<<distinct<<" tempdirs-left=";
  system(("ls -1 "+dir+" | wc -l").c_str());
  return 0;
}
