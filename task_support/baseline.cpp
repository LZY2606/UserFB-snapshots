#include <cal3d/corebone.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coreanimation.h>
#include <cal3d/mixer.h>
#include <cal3d/skeleton.h>
#include <cal3d/physique.h>
#include <cal3d/loader.h>
#include <cal3d/buffersource.h>
#include <cal3d/saver.h>
#include <cassert>
#include <cmath>
#include <iostream>
static bool near(float a,float b){return std::fabs(a-b)<1e-5;}
int main(){
 auto core=ISTD::make_shared<CalCoreSkeleton>();
 core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
 auto child=ISTD::make_shared<CalCoreBone>("tip",0);
 child->relativeTransform.translation=CalVector(2,0,0);
 child->inverseBindPoseTransform.translation=CalVector(-1,0,0);
 core->addCoreBone(child);
 auto clip=ISTD::make_shared<CalCoreAnimation>(); clip->duration=1;
 clip->tracks.push_back(CalCoreTrack(0,{{0,CalVector(3,0,0),CalQuaternion()},{1,CalVector(5,0,0),CalQuaternion()}}));
 std::string xml=CalSaver::saveCoreAnimationXmlToBuffer(clip);
 assert(!xml.empty());
 CalBufferSource input(xml.data(),xml.size());
 auto loaded=CalLoader::loadCoreAnimation(input); assert(loaded);
 CalSkeleton sk(core); CalMixer mixer;
 auto anim=ISTD::make_shared<CalAnimation>(loaded,1,0); anim->time=.5f;
 mixer.addAnimation(anim);
 mixer.updateSkeleton(&sk,{},{{0,CalVector(2,1,1)}});
 assert(near(sk.bones[1].absoluteTransform.translation.x,8));
 CalCoreSubmesh::Vertex v[]={{CalPoint4(1,0,0),CalVector4(0,1,0)}};
 CalCoreSubmesh::Influence inf[]={{0,.25f,false},{1,.75f,true}};
 CalVector4 out[2]; CalPhysique::calculateVerticesAndNormals_x87(&sk.boneTransforms[0],1,v,inf,out);
 assert(near(out[0].x,7.5f)); assert(near(out[1].y,1));
 mixer.removeAnimation(anim);mixer.updateSkeleton(&sk,{},{});
 assert(near(sk.bones[1].absoluteTransform.translation.x,2));
 assert(near(core->coreBones[1]->relativeTransform.translation.x,2));
 std::cout<<"CAL3D_BASELINE_OK xml=1 sampled_translation=4 adjusted_scale=2 skinned_x=7.5 reset=1\n";
}
