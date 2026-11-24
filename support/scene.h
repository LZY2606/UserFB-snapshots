#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
struct LayerFilter : JPH::ObjectVsBroadPhaseLayerFilter {
 bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b)const override {return a==1||b==JPH::BroadPhaseLayer(1);}
};
struct Scene {
 JPH::BroadPhaseLayerInterfaceTable bp{2,2}; JPH::ObjectLayerPairFilterTable pairs{2};
 JPH::TempAllocatorImpl temp{8*1024*1024}; JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
 JPH::PhysicsSystem world;
 LayerFilter filter;
 Scene() {
  bp.MapObjectToBroadPhaseLayer(0,JPH::BroadPhaseLayer(0));bp.MapObjectToBroadPhaseLayer(1,JPH::BroadPhaseLayer(1));
  pairs.EnableCollision(0,1);pairs.EnableCollision(1,1);
  world.Init(64,0,128,128,bp,filter,pairs);
 }
 JPH::BodyID box(JPH::Vec3 half,JPH::RVec3 pos,JPH::EMotionType type,JPH::Vec3 velocity=JPH::Vec3::sZero()) {
  JPH::BodyCreationSettings settings(new JPH::BoxShape(half),pos,JPH::Quat::sIdentity(),type,type==JPH::EMotionType::Static?0:1);
  settings.mLinearVelocity=velocity;settings.mAllowSleeping=false;settings.mFriction=0.5f;
  return world.GetBodyInterface().CreateAndAddBody(settings,JPH::EActivation::Activate);
 }
 void step(){world.Update(1.0f/120,1,&temp,&jobs);}
};
