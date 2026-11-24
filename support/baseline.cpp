#include "scene.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
 JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory();JPH::RegisterTypes();
 {
  Scene s;s.box(JPH::Vec3(10,.5f,10),JPH::RVec3(0,-.5f,0),JPH::EMotionType::Static);
  auto b=s.box(JPH::Vec3(.5f,.5f,.5f),JPH::RVec3(0,.5f,0),JPH::EMotionType::Dynamic,JPH::Vec3(4,0,1));
  for(int i=0;i<120;i++)s.step();
  auto &bi=s.world.GetBodyInterface();auto p=bi.GetPosition(b);auto v=bi.GetLinearVelocity(b);
  assert(std::isfinite(p.GetY())&&p.GetY()>.47f&&p.GetY()<.55f);assert(v.Length()<.1f);
  std::printf("BASELINE sliding-box PASS y=%.6f speed=%.6f\n",double(p.GetY()),double(v.Length()));
 }
 JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;
}
