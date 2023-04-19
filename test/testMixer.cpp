#include "TestPrologue.h"
#include <cal3d/corebone.h>
#include <cal3d/coreanimation.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/mixer.h>
#include <cal3d/skeleton.h>

FIXTURE(MixerFixture) {
    SETUP(MixerFixture)
        : coreSkeleton(fakeMixerSkeleton())
        , skeleton(coreSkeleton)
    {}

    static boost::shared_ptr<CalCoreSkeleton> fakeMixerSkeleton() {
        boost::shared_ptr<CalCoreSkeleton> p(new CalCoreSkeleton());
        CalCoreBonePtr root(new CalCoreBone("root"));
        p->addCoreBone(root);
        return p;
    }

    void updateSkeleton() {
        mixer.updateSkeleton(
            &skeleton,
            std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());
    }

    boost::shared_ptr<CalCoreSkeleton> coreSkeleton;
    CalSkeleton skeleton;
    CalMixer mixer;
};

TEST_F(MixerFixture, no_animation_leaves_bone_in_bind_pose) {
    updateSkeleton();
    CHECK_EQUAL(cal3d::Transform(), skeleton.bones[0].absoluteTransform);
}

static boost::shared_ptr<CalAnimation> makeAnimation(CalVector t, unsigned priority, float weight = 1.0f) {
    CalCoreTrack::KeyframeList keyframes;
    keyframes.push_back(CalCoreKeyframe(0, t, CalQuaternion()));
    CalCoreTrack track(0, keyframes);

    boost::shared_ptr<CalCoreAnimation> coreAnimation(new CalCoreAnimation());
    coreAnimation->tracks.push_back(track);

    boost::shared_ptr<CalAnimation> anim(new CalAnimation(coreAnimation, weight, priority));
    return anim;
}

TEST_F(MixerFixture, one_low_priority_animation_poses_bone) {
    boost::shared_ptr<CalAnimation> anim(makeAnimation(CalVector(1, 1, 1), 0));

    mixer.addAnimation(anim);
    updateSkeleton();
    CHECK_EQUAL(CalVector(1, 1, 1), skeleton.bones[0].absoluteTransform.translation);
}

TEST_F(MixerFixture, high_priority_animation_trumps_low_priority_animation) {
    boost::shared_ptr<CalAnimation> low(makeAnimation(CalVector(1, 1, 1), 0));
    boost::shared_ptr<CalAnimation> high(makeAnimation(CalVector(-1, -1, -1), 1));

    mixer.addAnimation(low);
    mixer.addAnimation(high);

    updateSkeleton();
    CHECK_EQUAL(CalVector(-1, -1, -1), skeleton.bones[0].absoluteTransform.translation);
}

TEST_F(MixerFixture, high_priority_can_be_inserted_before_low_priority_animation) {
    boost::shared_ptr<CalAnimation> low(makeAnimation(CalVector(1, 1, 1), 0));
    boost::shared_ptr<CalAnimation> high(makeAnimation(CalVector(-1, -1, -1), 2));

    mixer.addAnimation(high);
    mixer.addAnimation(low);

    updateSkeleton();
    CHECK_EQUAL(CalVector(-1, -1, -1), skeleton.bones[0].absoluteTransform.translation);
}

TEST_F(MixerFixture, second_high_priority_trumps_first) {
    boost::shared_ptr<CalAnimation> first(makeAnimation(CalVector(1, 1, 1), 2));
    boost::shared_ptr<CalAnimation> second(makeAnimation(CalVector(-1, -1, -1), 2));

    mixer.addAnimation(first);
    mixer.addAnimation(second);

    updateSkeleton();
    CHECK_EQUAL(CalVector(-1, -1, -1), skeleton.bones[0].absoluteTransform.translation);
}
