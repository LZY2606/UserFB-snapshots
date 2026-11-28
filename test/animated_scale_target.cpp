//****************************************************************************//
// animated_scale_target.cpp                                                  //
//                                                                            //
// Tests for keyframed squash & stretch: CalCoreKeyframe::localScale is       //
// sampled by CalCoreTrack::getCurrentScale, blended by CalMixer into         //
// CalBone::scale, and reaches skinning through the existing parent/child     //
// scale propagation, inverse bind and normal handling.                       //
//****************************************************************************//

#include "task_support/check.h"

#include <cal3d/animation.h>
#include <cal3d/buffersource.h>
#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coretrack.h>
#include <cal3d/error.h>
#include <cal3d/loader.h>
#include <cal3d/mixer.h>
#include <cal3d/physique.h>
#include <cal3d/saver.h>
#include <cal3d/skeleton.h>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

static bool near(float a, float b) {
    return std::fabs(a - b) <= 1e-5f;
}

static bool nearVec(const CalVector& a, const CalVector& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

// Independent per-component linear interpolation reference.
static CalVector refLerp(float f, const CalVector& a, const CalVector& b) {
    return CalVector(
        a.x + (b.x - a.x) * f,
        a.y + (b.y - a.y) * f,
        a.z + (b.z - a.z) * f);
}

static CalQuaternion rotZ90() {
    CalQuaternion q;
    q.setAxisAngle(CalVector(0, 0, 1), 1.5707963267948966f);
    return q;
}

static ISTD::shared_ptr<CalCoreAnimation> loadAnim(const std::string& data) {
    CalBufferSource src(data.data(), data.size());
    return CalLoader::loadCoreAnimation(src);
}

// ---------------------------------------------------------------------------
// sampling_assets
// ---------------------------------------------------------------------------

static int case_scale_sampling_matches_independent_reference() {
    // Default and legacy constructors yield the unit scale.
    TASK_CHECK(nearVec(CalCoreKeyframe().localScale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(CalCoreKeyframe(0, CalVector(1, 2, 3), CalQuaternion()).localScale, CalVector(1, 1, 1)));

    CalCoreTrack::KeyframeList kfs;
    kfs.push_back(CalCoreKeyframe(0.5f, CalVector(1, 2, 3), CalQuaternion(), CalVector(2, 4, 8)));
    kfs.push_back(CalCoreKeyframe(1.5f, CalVector(3, 0, -3), CalQuaternion(), CalVector(4, 2, 2)));
    CalCoreTrack track(0, kfs);

    // Empty track samples the unit scale.
    CalCoreTrack emptyTrack(0, CalCoreTrack::KeyframeList());
    TASK_CHECK(nearVec(emptyTrack.getCurrentScale(0.3f), CalVector(1, 1, 1)));

    // First/last key values are held outside the key range.
    TASK_CHECK(nearVec(track.getCurrentScale(-1.0f), CalVector(2, 4, 8)));
    TASK_CHECK(nearVec(track.getCurrentScale(0.5f), CalVector(2, 4, 8)));
    TASK_CHECK(nearVec(track.getCurrentScale(1.5f), CalVector(4, 2, 2)));
    TASK_CHECK(nearVec(track.getCurrentScale(99.0f), CalVector(4, 2, 2)));

    // Interior samples follow an independent per-component linear reference.
    TASK_CHECK(nearVec(track.getCurrentScale(1.0f), refLerp(0.5f, CalVector(2, 4, 8), CalVector(4, 2, 2))));
    TASK_CHECK(nearVec(track.getCurrentScale(1.0f), CalVector(3, 3, 5)));
    TASK_CHECK(nearVec(track.getCurrentScale(0.75f), refLerp(0.25f, CalVector(2, 4, 8), CalVector(4, 2, 2))));
    TASK_CHECK(nearVec(track.getCurrentScale(1.25f), refLerp(0.75f, CalVector(2, 4, 8), CalVector(4, 2, 2))));

    // The existing translation/rotation sampling is unchanged.
    cal3d::RotateTranslate rt = track.getCurrentTransform(1.0f);
    TASK_CHECK(nearVec(rt.translation, CalVector(2, 1, 0)));
    TASK_CHECK(nearVec(track.getCurrentTransform(-1.0f).translation, CalVector(1, 2, 3)));
    TASK_CHECK(nearVec(track.getCurrentTransform(99.0f).translation, CalVector(3, 0, -3)));

    // scale(factor) remains a model-unit conversion: it only scales the
    // translation, never the dimensionless localScale channel.
    CalCoreKeyframe unitConversionKey(0, CalVector(2, 4, 6), CalQuaternion(), CalVector(2, 3, 4));
    unitConversionKey.scale(2.0f);
    TASK_CHECK(nearVec(unitConversionKey.transform.translation, CalVector(4, 8, 12)));
    TASK_CHECK(nearVec(unitConversionKey.localScale, CalVector(2, 3, 4)));

    CalCoreTrack scaledTrack(0, kfs);
    scaledTrack.scale(2.0f);
    TASK_CHECK(nearVec(scaledTrack.keyframes[0].transform.translation, CalVector(2, 4, 6)));
    TASK_CHECK(nearVec(scaledTrack.keyframes[0].localScale, CalVector(2, 4, 8)));
    TASK_CHECK(nearVec(scaledTrack.getCurrentScale(1.0f), CalVector(3, 3, 5)));
    return 0;
}

static int case_optimize_and_compress_keep_scale_channel() {
    // optimize(): two keys with equal transforms but different scales are not
    // collapsed - the scale-only motion must survive.
    {
        CalCoreTrack::KeyframeList kfs;
        kfs.push_back(CalCoreKeyframe(0, CalVector(1, 0, 0), CalQuaternion(), CalVector(1, 1, 1)));
        kfs.push_back(CalCoreKeyframe(1, CalVector(1, 0, 0), CalQuaternion(), CalVector(2, 2, 2)));
        CalCoreTrack track(0, kfs);
        track.optimize();
        TASK_CHECK(track.keyframes.size() == 2);
        TASK_CHECK(nearVec(track.getCurrentScale(0.5f), CalVector(1.5f, 1.5f, 1.5f)));
    }

    // optimize(): fully identical keys (including scale) still collapse, and
    // the surviving key keeps its scale.
    {
        CalCoreTrack::KeyframeList kfs;
        kfs.push_back(CalCoreKeyframe(0, CalVector(1, 0, 0), CalQuaternion(), CalVector(2, 2, 2)));
        kfs.push_back(CalCoreKeyframe(1, CalVector(1, 0, 0), CalQuaternion(), CalVector(2, 2, 2)));
        CalCoreTrack track(0, kfs);
        track.optimize();
        TASK_CHECK(track.keyframes.size() == 1);
        TASK_CHECK(nearVec(track.keyframes[0].localScale, CalVector(2, 2, 2)));
    }

    // compress(): keys that only differ in scale, non-linearly, must all be
    // kept (the scale channel may not be skipped).
    {
        CalCoreTrack::KeyframeList kfs;
        const float scales[5] = {1, 2, 1, 2, 1};
        for (int i = 0; i < 5; ++i) {
            kfs.push_back(CalCoreKeyframe(
                (float)i, CalVector(1, 0, 0), CalQuaternion(),
                CalVector(scales[i], scales[i], scales[i])));
        }
        CalCoreTrack track(0, kfs);
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.01, 0.5, 0);
        TASK_CHECK(compressed->keyframes.size() == 5);
        TASK_CHECK(nearVec(compressed->getCurrentScale(1.0f), CalVector(2, 2, 2)));
        TASK_CHECK(nearVec(compressed->getCurrentScale(2.0f), CalVector(1, 1, 1)));
    }

    // compress(): truly linearly redundant scale keys are removed, and the
    // surviving keys still reproduce the ramp.
    {
        CalCoreTrack::KeyframeList kfs;
        for (int i = 0; i < 5; ++i) {
            float s = (float)(i + 1);
            kfs.push_back(CalCoreKeyframe(
                (float)i, CalVector(1, 0, 0), CalQuaternion(), CalVector(s, s, s)));
        }
        CalCoreTrack track(0, kfs);
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.01, 0.5, 0);
        TASK_CHECK(compressed->keyframes.size() == 2);
        TASK_CHECK(nearVec(compressed->keyframes[0].localScale, CalVector(1, 1, 1)));
        TASK_CHECK(nearVec(compressed->keyframes[1].localScale, CalVector(5, 5, 5)));
        TASK_CHECK(nearVec(compressed->getCurrentScale(2.0f), CalVector(3, 3, 3)));
    }

    // compress(): a single non-linear component is enough to keep a key.
    {
        CalCoreTrack::KeyframeList kfs;
        kfs.push_back(CalCoreKeyframe(0, CalVector(1, 0, 0), CalQuaternion(), CalVector(1, 1, 1)));
        kfs.push_back(CalCoreKeyframe(1, CalVector(1, 0, 0), CalQuaternion(), CalVector(2, 1.4f, 2)));
        kfs.push_back(CalCoreKeyframe(2, CalVector(1, 0, 0), CalQuaternion(), CalVector(3, 2, 3)));
        CalCoreTrack track(0, kfs);
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.01, 0.5, 0);
        TASK_CHECK(compressed->keyframes.size() == 3);
    }
    return 0;
}

static int case_xml_roundtrip_and_binary_rejection() {
    // Build an animation whose keyframes carry non-unit scales.
    ISTD::shared_ptr<CalCoreAnimation> anim = ISTD::make_shared<CalCoreAnimation>();
    anim->duration = 1.0f;
    CalCoreTrack::KeyframeList kfs;
    CalQuaternion rot;
    rot.setAxisAngle(CalVector(0, 0, 1), 0.5f);
    kfs.push_back(CalCoreKeyframe(0, CalVector(1, 2, 3), CalQuaternion(), CalVector(1, 1, 1)));
    kfs.push_back(CalCoreKeyframe(1, CalVector(2, 4, 6), rot, CalVector(2, 3, 4)));
    anim->tracks.push_back(CalCoreTrack(0, kfs));

    // XML roundtrip preserves translation, rotation and scale.
    std::string xml = CalSaver::saveCoreAnimationXmlToBuffer(anim);
    TASK_CHECK(!xml.empty());
    TASK_CHECK(xml.find("SCALE") != std::string::npos);
    ISTD::shared_ptr<CalCoreAnimation> loaded = loadAnim(xml);
    TASK_CHECK(loaded);
    TASK_CHECK(loaded->tracks.size() == 1);
    TASK_CHECK(loaded->tracks[0].keyframes.size() == 2);
    const CalCoreKeyframe& k0 = loaded->tracks[0].keyframes[0];
    const CalCoreKeyframe& k1 = loaded->tracks[0].keyframes[1];
    TASK_CHECK(nearVec(k0.localScale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(k1.localScale, CalVector(2, 3, 4)));
    TASK_CHECK(nearVec(k1.transform.translation, CalVector(2, 4, 6)));
    TASK_CHECK(near(k1.transform.rotation.z, rot.z));
    TASK_CHECK(near(k1.transform.rotation.w, rot.w));

    // Old XML without any SCALE element still loads, with unit scales.
    const char* oldXml =
        "<HEADER MAGIC=\"XAF\" VERSION=\"919\"/>"
        "<ANIMATION DURATION=\"1\" NUMTRACKS=\"1\">"
        "<TRACK BONEID=\"0\" NUMKEYFRAMES=\"1\">"
        "<KEYFRAME TIME=\"0\">"
        "<TRANSLATION>1 2 3</TRANSLATION>"
        "<ROTATION>0 0 0 -1</ROTATION>"
        "</KEYFRAME>"
        "</TRACK>"
        "</ANIMATION>";
    ISTD::shared_ptr<CalCoreAnimation> oldLoaded = loadAnim(oldXml);
    TASK_CHECK(oldLoaded);
    TASK_CHECK(oldLoaded->tracks.size() == 1);
    TASK_CHECK(nearVec(oldLoaded->tracks[0].keyframes[0].localScale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(oldLoaded->tracks[0].keyframes[0].transform.translation, CalVector(1, 2, 3)));

    // Malformed SCALE elements must fail to load: missing component,
    // non-positive, and non-finite values.
    const char* badScales[] = {
        "<SCALE>1 2</SCALE>",
        "<SCALE>0 1 1</SCALE>",
        "<SCALE>1 -1 1</SCALE>",
        "<SCALE>nan 1 1</SCALE>",
        "<SCALE>1 1 inf</SCALE>",
    };
    for (size_t i = 0; i < sizeof(badScales) / sizeof(badScales[0]); ++i) {
        std::string bad =
            "<HEADER MAGIC=\"XAF\" VERSION=\"919\"/>"
            "<ANIMATION DURATION=\"1\" NUMTRACKS=\"1\">"
            "<TRACK BONEID=\"0\" NUMKEYFRAMES=\"1\">"
            "<KEYFRAME TIME=\"0\">"
            "<TRANSLATION>1 2 3</TRANSLATION>"
            "<ROTATION>0 0 0 -1</ROTATION>";
        bad += badScales[i];
        bad +=
            "</KEYFRAME>"
            "</TRACK>"
            "</ANIMATION>";
        TASK_CHECK(!loadAnim(bad));
    }

    // The binary CAF entry point refuses non-unit scales before writing any
    // byte, and reports the failure through CalError.
    std::ostringstream os;
    TASK_CHECK(!CalSaver::saveCoreAnimation(os, anim.get()));
    TASK_CHECK(os.str().empty());
    TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_ATTRIBUTE_VALUE);
    TASK_CHECK(CalSaver::saveCoreAnimationToBuffer(anim).empty());

    // Unit-scale animations still save and load as CAF; the loaded keyframes
    // carry the unit scale.
    ISTD::shared_ptr<CalCoreAnimation> unitAnim = ISTD::make_shared<CalCoreAnimation>();
    unitAnim->duration = 1.0f;
    CalCoreTrack::KeyframeList unitKfs;
    unitKfs.push_back(CalCoreKeyframe(0, CalVector(1, 2, 3), CalQuaternion()));
    unitKfs.push_back(CalCoreKeyframe(1, CalVector(2, 4, 6), CalQuaternion()));
    unitAnim->tracks.push_back(CalCoreTrack(0, unitKfs));
    std::string caf = CalSaver::saveCoreAnimationToBuffer(unitAnim);
    TASK_CHECK(!caf.empty());
    ISTD::shared_ptr<CalCoreAnimation> cafLoaded = loadAnim(caf);
    TASK_CHECK(cafLoaded);
    TASK_CHECK(cafLoaded->tracks.size() == 1);
    TASK_CHECK(cafLoaded->tracks[0].keyframes.size() == 2);
    TASK_CHECK(nearVec(cafLoaded->tracks[0].keyframes[0].localScale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(cafLoaded->tracks[0].keyframes[1].localScale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(cafLoaded->tracks[0].keyframes[1].transform.translation, CalVector(2, 4, 6)));
    return 0;
}

// ---------------------------------------------------------------------------
// priority_frames
// ---------------------------------------------------------------------------

// Minimal usage example: keyframe a scale on one bone and let the mixer
// evaluate it into the skeleton.
//
//   CalCoreTrack::KeyframeList keys;
//   keys.push_back(CalCoreKeyframe(0, CalVector(), CalQuaternion(), CalVector(1, 1, 1)));
//   keys.push_back(CalCoreKeyframe(1, CalVector(), CalQuaternion(), CalVector(2, 1, 1)));
//   ISTD::shared_ptr<CalCoreAnimation> clip = ISTD::make_shared<CalCoreAnimation>();
//   clip->duration = 1;
//   clip->tracks.push_back(CalCoreTrack(0, keys));
//   ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
//   anim->time = 0.5f;
//   CalMixer mixer;
//   mixer.addAnimation(anim);
//   mixer.updateSkeleton(&skeleton, {}, {});   // skeleton.bones[0].scale == (1.5, 1, 1)
//
static int case_minimal_usage_example() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));

    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(0, CalVector(), CalQuaternion(), CalVector(1, 1, 1)));
    keys.push_back(CalCoreKeyframe(1, CalVector(), CalQuaternion(), CalVector(2, 1, 1)));
    ISTD::shared_ptr<CalCoreAnimation> clip = ISTD::make_shared<CalCoreAnimation>();
    clip->duration = 1;
    clip->tracks.push_back(CalCoreTrack(0, keys));

    ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
    anim->time = 0.5f;

    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.addAnimation(anim);
    mixer.updateSkeleton(&skeleton, {}, {});

    TASK_CHECK(nearVec(skeleton.bones[0].scale.scale, CalVector(1.5f, 1, 1)));
    return 0;
}

static ISTD::shared_ptr<CalAnimation> makeScaleAnimation(
    unsigned priority,
    float rampValue,
    float scaleBone0,
    float translationX0,
    bool hasBone1Track,
    float scaleBone1
) {
    ISTD::shared_ptr<CalCoreAnimation> core = ISTD::make_shared<CalCoreAnimation>();
    core->duration = 1.0f;
    CalCoreTrack::KeyframeList k0;
    k0.push_back(CalCoreKeyframe(
        0, CalVector(translationX0, 0, 0), CalQuaternion(),
        CalVector(scaleBone0, scaleBone0, scaleBone0)));
    core->tracks.push_back(CalCoreTrack(0, k0));
    if (hasBone1Track) {
        CalCoreTrack::KeyframeList k1;
        k1.push_back(CalCoreKeyframe(
            0, CalVector(0, 0, 0), CalQuaternion(),
            CalVector(scaleBone1, scaleBone1, scaleBone1)));
        core->tracks.push_back(CalCoreTrack(1, k1));
    }
    ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(core, 1.0f, priority);
    anim->rampValue = rampValue;
    return anim;
}

static int case_high_and_low_priority_blend_scale() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root0"));
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root1", 0));

    // Inserted out of order; the mixer list keeps highest priority first, so
    // the processing order is C (p2), A (p1), B (p1).
    ISTD::shared_ptr<CalAnimation> animC = makeScaleAnimation(2, 0.5f, 8, 9, true, 8);
    ISTD::shared_ptr<CalAnimation> animB = makeScaleAnimation(1, 0.5f, 4, 3, true, 4);
    ISTD::shared_ptr<CalAnimation> animA = makeScaleAnimation(1, 0.5f, 2, 1, false, 0);

    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.addAnimation(animC);
    mixer.addAnimation(animB);
    mixer.addAnimation(animA);
    mixer.updateSkeleton(&skeleton, {}, {});

    // Bone 0: effective weights 0.5 (C), 0.25 (A after C's attenuation),
    // 0.125 (B).  Scale = (8*0.5 + 2*0.25 + 4*0.125) / 0.875 = 40/7.
    TASK_CHECK(nearVec(skeleton.bones[0].scale.scale, CalVector(40.0f / 7, 40.0f / 7, 40.0f / 7)));

    // Bone 1: A has no track for it, so A does not consume its remaining
    // coefficient.  Weights 0.5 (C), 0.25 (B): (8*0.5 + 4*0.25) / 0.75 = 20/3.
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(20.0f / 3, 20.0f / 3, 20.0f / 3)));

    // The pose channel keeps its existing blending behavior.
    TASK_CHECK(near(skeleton.bones[0].absoluteTransform.translation.x, 41.0f / 7));
    return 0;
}

static int case_explicit_adjustments_combine_with_animation_scale() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform.translation = CalVector(7, 0, 0);
    core->addCoreBone(child);

    ISTD::shared_ptr<CalCoreAnimation> clip = ISTD::make_shared<CalCoreAnimation>();
    clip->duration = 1.0f;
    CalCoreTrack::KeyframeList kfs;
    kfs.push_back(CalCoreKeyframe(0, CalVector(5, 0, 0), CalQuaternion(), CalVector(2, 2, 2)));
    clip->tracks.push_back(CalCoreTrack(1, kfs));
    ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);

    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.addAnimation(anim);

    // The transform adjustment replaces the pose at full ramp but must not
    // depress the animation's scale weight.
    BoneTransformAdjustment poseOverride;
    poseOverride.boneId = 1;
    poseOverride.localOri = rotZ90();
    poseOverride.rampValue = 1.0f;

    std::vector<BoneTransformAdjustment> poseAdjustments(1, poseOverride);
    std::vector<BoneScaleAdjustment> scaleAdjustments;
    scaleAdjustments.push_back(BoneScaleAdjustment(1, CalVector(3, 1, 0.5f)));

    mixer.updateSkeleton(&skeleton, poseAdjustments, scaleAdjustments);

    // Animated scale (2,2,2) multiplied component-wise by the adjustment.
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(6, 2, 1)));

    // The pose comes from the adjustment, not from the animation track.
    TASK_CHECK(nearVec(skeleton.bones[1].absoluteTransform.translation, CalVector(7, 0, 0)));
    TASK_CHECK(nearVec(skeleton.bones[1].absoluteTransform.basis * CalVector(1, 0, 0), CalVector(0, 6, 0)));

    // The last scale adjustment for a bone wins; earlier ones are dropped.
    std::vector<BoneScaleAdjustment> twoAdjustments;
    twoAdjustments.push_back(BoneScaleAdjustment(1, CalVector(9, 9, 9)));
    twoAdjustments.push_back(BoneScaleAdjustment(1, CalVector(3, 1, 0.5f)));
    mixer.updateSkeleton(&skeleton, poseAdjustments, twoAdjustments);
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(6, 2, 1)));

    // Without an explicit adjustment the plain animated scale remains.
    mixer.updateSkeleton(&skeleton, poseAdjustments, std::vector<BoneScaleAdjustment>());
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(2, 2, 2)));
    return 0;
}

static int case_removal_and_repeated_updates_leave_no_residue() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform.translation = CalVector(1, 2, 3);
    core->addCoreBone(child);

    ISTD::shared_ptr<CalCoreAnimation> clip = ISTD::make_shared<CalCoreAnimation>();
    clip->duration = 1.0f;
    CalCoreTrack::KeyframeList kfs;
    kfs.push_back(CalCoreKeyframe(0, CalVector(4, 5, 6), CalQuaternion(), CalVector(2, 3, 4)));
    clip->tracks.push_back(CalCoreTrack(1, kfs));
    ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);

    CalSkeleton skeleton1(core);
    CalSkeleton skeleton2(core);
    CalMixer mixer;
    mixer.addAnimation(anim);

    // Evaluating the same time repeatedly gives the same scale.
    mixer.updateSkeleton(&skeleton1, {}, {});
    TASK_CHECK(nearVec(skeleton1.bones[1].scale.scale, CalVector(2, 3, 4)));
    TASK_CHECK(nearVec(skeleton1.bones[1].absoluteTransform.translation, CalVector(4, 5, 6)));
    mixer.updateSkeleton(&skeleton1, {}, {});
    TASK_CHECK(nearVec(skeleton1.bones[1].scale.scale, CalVector(2, 3, 4)));

    // Updating another character with the same mixer works and does not
    // disturb the first character's state.
    mixer.updateSkeleton(&skeleton2, {}, {});
    TASK_CHECK(nearVec(skeleton2.bones[1].scale.scale, CalVector(2, 3, 4)));
    TASK_CHECK(nearVec(skeleton1.bones[1].scale.scale, CalVector(2, 3, 4)));

    // Removing the animation restores the unit scale and bind pose.
    mixer.removeAnimation(anim);
    mixer.updateSkeleton(&skeleton1, {}, {});
    TASK_CHECK(nearVec(skeleton1.bones[1].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(skeleton1.bones[1].absoluteTransform.translation, CalVector(1, 2, 3)));
    mixer.updateSkeleton(&skeleton2, {}, {});
    TASK_CHECK(nearVec(skeleton2.bones[1].scale.scale, CalVector(1, 1, 1)));
    return 0;
}

// ---------------------------------------------------------------------------
// skinning
// ---------------------------------------------------------------------------

static int case_scaled_bones_reach_skinning_output() {
    // Root bone with identity bind; child bone rotated 90 degrees about Z
    // with a non-identity inverse bind translation.
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform.rotation = rotZ90();
    child->relativeTransform.translation = CalVector(2, 0, 0);
    child->inverseBindPoseTransform.translation = CalVector(-1, -2, 0);
    core->addCoreBone(child);

    ISTD::shared_ptr<CalCoreAnimation> clip = ISTD::make_shared<CalCoreAnimation>();
    clip->duration = 1.0f;
    CalCoreTrack::KeyframeList k0;
    k0.push_back(CalCoreKeyframe(0, CalVector(0, 0, 0), CalQuaternion(), CalVector(1, 1, 1)));
    k0.push_back(CalCoreKeyframe(1, CalVector(0, 0, 0), CalQuaternion(), CalVector(2, 1, 1)));
    clip->tracks.push_back(CalCoreTrack(0, k0));
    CalCoreTrack::KeyframeList k1;
    k1.push_back(CalCoreKeyframe(0, CalVector(2, 0, 0), rotZ90(), CalVector(1, 1, 1)));
    k1.push_back(CalCoreKeyframe(1, CalVector(2, 0, 0), rotZ90(), CalVector(1, 3, 1)));
    clip->tracks.push_back(CalCoreTrack(1, k1));

    ISTD::shared_ptr<CalAnimation> anim = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
    anim->time = 0.5f;

    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.addAnimation(anim);
    mixer.updateSkeleton(&skeleton, {}, {});

    // The animated scales reach the bones: root (1.5,1,1), child (1,2,1).
    TASK_CHECK(nearVec(skeleton.bones[0].scale.scale, CalVector(1.5f, 1, 1)));
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(1, 2, 1)));
    // Parent scale propagates to the child's absolute translation.
    TASK_CHECK(nearVec(skeleton.bones[1].absoluteTransform.translation, CalVector(3, 0, 0)));

    // Skin a point influenced by both bones through the scalar physique
    // entry point.
    CalCoreSubmesh::Vertex vertices[] = {{CalPoint4(1, 1, 1), CalVector4(0, 1, 0)}};
    CalCoreSubmesh::Influence influences[] = {
        CalCoreSubmesh::Influence(0, 0.25f, false),
        CalCoreSubmesh::Influence(1, 0.75f, true),
    };
    CalVector4 out[2];
    CalPhysique::calculateVerticesAndNormals_x87(&skeleton.boneTransforms[0], 1, vertices, influences, out);

    // Independently computed reference:
    //   BT0 = (diag(1.5,1,1), (0,0,0))
    //   BT1 = (Rz90 * diag(1.5,2,1), (7,-1.5,0))
    //   out = 0.25 * BT0 * p + 0.75 * BT1 * p
    TASK_CHECK(near(out[0].x, 4.125f));
    TASK_CHECK(near(out[0].y, 0.25f));
    TASK_CHECK(near(out[0].z, 1.0f));
    TASK_CHECK(near(out[1].x, -1.5f));
    TASK_CHECK(near(out[1].y, 0.25f));
    TASK_CHECK(near(out[1].z, 0.0f));

    // At t=0 the scales are unit and the plain translation/rotation behavior
    // is preserved: BT0 = identity, BT1 = (Rz90, (4,-1,0)).
    anim->time = 0.0f;
    mixer.updateSkeleton(&skeleton, {}, {});
    TASK_CHECK(nearVec(skeleton.bones[0].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(skeleton.bones[1].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(nearVec(skeleton.bones[1].absoluteTransform.translation, CalVector(2, 0, 0)));
    CalPhysique::calculateVerticesAndNormals_x87(&skeleton.boneTransforms[0], 1, vertices, influences, out);
    TASK_CHECK(near(out[0].x, 2.5f));
    TASK_CHECK(near(out[0].y, 0.25f));
    TASK_CHECK(near(out[0].z, 1.0f));
    TASK_CHECK(near(out[1].x, -0.75f));
    TASK_CHECK(near(out[1].y, 0.25f));
    TASK_CHECK(near(out[1].z, 0.0f));
    return 0;
}

const struct task_case task_cases[] = {
    {"sampling_assets", "scale_sampling_matches_independent_reference", case_scale_sampling_matches_independent_reference},
    {"sampling_assets", "optimize_and_compress_keep_scale_channel", case_optimize_and_compress_keep_scale_channel},
    {"sampling_assets", "xml_roundtrip_and_binary_rejection", case_xml_roundtrip_and_binary_rejection},
    {"priority_frames", "minimal_usage_example", case_minimal_usage_example},
    {"priority_frames", "high_and_low_priority_blend_scale", case_high_and_low_priority_blend_scale},
    {"priority_frames", "explicit_adjustments_combine_with_animation_scale", case_explicit_adjustments_combine_with_animation_scale},
    {"priority_frames", "removal_and_repeated_updates_leave_no_residue", case_removal_and_repeated_updates_leave_no_residue},
    {"skinning", "scaled_bones_reach_skinning_output", case_scaled_bones_reach_skinning_output},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
