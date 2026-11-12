#include "headless.h"
#include <iostream>
int main(){
 const char* json=R"({"version":"5.5","compatibleVersion":"5.5","frameRate":24,"name":"data","armature":[{"name":"rig","bone":[{"name":"root"},{"name":"tip","parent":"root","transform":{"x":3}}],"animation":[{"name":"walk","duration":24,"playTimes":0,"bone":[{"name":"root","translateFrame":[{"duration":12,"x":0,"tweenEasing":0},{"duration":12,"x":8,"tweenEasing":0}]}]}]}]})";
 JSONDataParser parser;auto* data=parser.parseDragonBonesData(json,1);assert(data);
 HeadlessProxy proxy;DragonBones runtime(&proxy);auto* ad=data->getArmature("rig");assert(ad);
 auto* arm=BaseObject::borrowObject<Armature>();arm->init(ad,&proxy,nullptr,&runtime);
 for(auto* bd:ad->sortedBones){auto* b=BaseObject::borrowObject<Bone>();b->init(bd,arm);}
 arm->getAnimation()->setAnimations(ad->animations);
 auto* state=arm->getAnimation()->play("walk",0);assert(state);arm->advanceTime(.25f);
 auto* root=arm->getBone("root");auto* tip=arm->getBone("tip");
 assert(std::fabs(root->globalTransformMatrix.tx-4)<1e-5);
 assert(std::fabs(tip->globalTransformMatrix.tx-7)<1e-5);
 assert(std::fabs(state->getCurrentTime()-.25f)<1e-5);
 std::cout<<"DRAGONBONES_POSE_OK root="<<root->globalTransformMatrix.tx<<" tip="<<tip->globalTransformMatrix.tx<<"\n";
 arm->returnToPool();data->returnToPool();BaseObject::clearPool();
}
