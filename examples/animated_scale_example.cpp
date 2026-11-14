// Minimal example: animation-driven bone scale.
//
// Build (from the repository root, after the library is built by
// task_support/build.sh or your normal build):
//
//   c++ -std=c++17 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc \
//       examples/animated_scale_example.cpp .task-build/libcal3d.a \
//       -include algorithm -include cstring -o /tmp/animated_scale_example
//
// The key points:
//   * CalCoreKeyframe::localScale is a dimensionless, per-component bone
//     scale carried alongside translation/rotation in each keyframe.
//   * CalCoreTrack::getCurrentScale(time) linearly samples it.
//   * CalMixer::updateSkeleton blends it into CalBone::scale, so it reaches
//     CalSkeleton::boneTransforms and therefore skinning with no extra work.
//   * BoneScaleAdjustment is still available as a per-frame multiplier on top
//     of the animation scale.

#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coretrack.h>
#include <cal3d/mixer.h>
#include <cal3d/skeleton.h>

#include <cstdio>

int main() {
    // One bone whose keyframed scale grows from (1,1,1) to (2,2,2).
    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(0.0f, CalVector(), CalQuaternion(), CalVector(1, 1, 1)));
    keys.push_back(CalCoreKeyframe(1.0f, CalVector(), CalQuaternion(), CalVector(2, 2, 2)));

    CalCoreTrack track(0, keys);
    std::printf("sampled scale at t=0.5: %.2f %.2f %.2f\n",
                track.getCurrentScale(0.5f).x,
                track.getCurrentScale(0.5f).y,
                track.getCurrentScale(0.5f).z);

    ISTD::shared_ptr<CalCoreAnimation> coreAnimation = ISTD::make_shared<CalCoreAnimation>();
    coreAnimation->duration = 1.0f;
    coreAnimation->tracks.push_back(track);

    ISTD::shared_ptr<CalCoreSkeleton> coreSkeleton = ISTD::make_shared<CalCoreSkeleton>();
    coreSkeleton->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));

    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;

    ISTD::shared_ptr<CalAnimation> animation =
        ISTD::make_shared<CalAnimation>(coreAnimation, 1.0f, 0);
    animation->time = 0.5f;
    mixer.addAnimation(animation);

    // No explicit scale adjustment is needed any more; the animation supplies
    // it.  Pass a BoneScaleAdjustment only for per-frame overrides.
    mixer.updateSkeleton(
        &skeleton,
        std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    const cal3d::Scale& boneScale = skeleton.bones[0].scale;
    std::printf("bone scale from mixer:   %.2f %.2f %.2f\n",
                boneScale.scale.x,
                boneScale.scale.y,
                boneScale.scale.z);
    return 0;
}
