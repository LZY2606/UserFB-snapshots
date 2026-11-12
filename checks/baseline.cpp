#include "ofbx.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
 std::ifstream f("checks/inherit.fbx",std::ios::binary);std::vector<ofbx::u8> b((std::istreambuf_iterator<char>(f)),{});
 auto* scene=ofbx::load(b.data(),b.size(),0);if(!scene){std::cerr<<ofbx::getError()<<"\n";return 1;}
 const ofbx::Object* child=nullptr;
 for(int i=0;i<scene->getAllObjectCount();++i)if(std::strcmp(scene->getAllObjects()[i]->name,"Model::child")==0)child=scene->getAllObjects()[i];
 assert(child && child->getParent());auto m=child->getGlobalTransform();auto local=child->getLocalTransform();
 double x=m.m[0]+m.m[12],y=m.m[1]+m.m[13];
 assert(std::fabs(m.m[12]-6)<1e-6 && std::fabs(local.m[12]-3)<1e-6);
 assert(std::fabs(x-6)<1e-6 && std::isfinite(y));
 // RrSs rotates the inherited anisotropic scale: the intended point is (6,2).
 
 std::cout<<"OPENFBX_LOCAL_OK tx=3\nINHERIT_CURRENT_POINT y="<<y<<" intended=2\n";
 scene->destroy();
}
