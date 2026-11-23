#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "Util.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <iostream>
static void require(bool b,const char *s){if(!b){std::cerr<<"BASELINE_FAIL "<<s<<"\n";std::exit(1);}}
static void check(VoxelOctree& tree){
 uint32 n=0;float t=-1;
 uint32 left=compressMaterial(Vec3(1,0,0),.6f),right=compressMaterial(Vec3(0,1,0),.8f);
 require(tree.raymarch(Vec3(0,1.21875f,1.28125f),Vec3(1,.001f,.001f),0,n,t),"left hit");
 require(n==left && std::fabs(t-1.125f)<1e-5,"left material distance");
 require(tree.raymarch(Vec3(3,1.21875f,1.28125f),Vec3(-1,.001f,.001f),0,n,t),"right hit");
 require(n==right && std::fabs(t-1.375f)<1e-5,"right material distance");
 require(!tree.raymarch(Vec3(0,1.1f,1.1f),Vec3(1,.001f,.001f),0,n,t),"empty ray");
}
int main(){
 ThreadUtils::startThreads(2);
 FILE *f=std::fopen(".task-build/scene.raw","wb");require(f,"raw file");
 int dim[3]={16,12,10};std::fwrite(dim,sizeof(int),3,f);
 uint32 a=compressMaterial(Vec3(1,0,0),.6f),b=compressMaterial(Vec3(0,1,0),.8f);
 for(int z=0;z<10;++z)for(int y=0;y<12;++y)for(int x=0;x<16;++x){
  uint32 v=(x==2&&y==3&&z==4)?a:((x==9&&y==3&&z==4)?b:0);std::fwrite(&v,4,1,f);
 }std::fclose(f);
 VoxelData vox(".task-build/scene.raw",2048);VoxelOctree tree(&vox);check(tree);
 require(std::fabs(tree.center().y-.375f)<1e-6,"noncube center");
 tree.save(".task-build/scene.oct");VoxelOctree loaded(".task-build/scene.oct");check(loaded);
 delete ThreadUtils::pool;ThreadUtils::pool=nullptr;
 std::cout<<"BASELINE_SVO_OK raw=16x12x10 cache=2048 left_t=1.125 right_t=1.375 roundtrip=1\n";
}
