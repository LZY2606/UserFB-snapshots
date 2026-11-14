// Animated (keyframe-driven) bone scale coverage.
//
// All assets used here are constructed in code on purpose: the task harness
// compiles only production sources and this file, and must not depend on any
// checked-in animation files.

#include "task_support/check.h"

#include <cal3d/animation.h>
#include <cal3d/bone.h>
#include <cal3d/bonetransform.h>
#include <cal3d/buffersource.h>
#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/coretrack.h>
#include <cal3d/error.h>
#include <cal3d/loader.h>
#include <cal3d/matrix.h>
#include <cal3d/mixer.h>
#include <cal3d/physique.h>
#include <cal3d/quaternion.h>
#include <cal3d/saver.h>
#include <cal3d/skeleton.h>
#include <cal3d/transform.h>
#include <cal3d/vector.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

const float kTol = 1e-5f;

bool near(float a, float b, float tol = kTol) {
    return std::fabs(a - b) <= tol;
}

bool vecNear(const CalVector& a, const CalVector& b, float tol = kTol) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol);
}

CalQuaternion axisAngle(const CalVector& axis, float radians) {
    CalQuaternion q;
    q.setAxisAngle(axis, radians);
    return q;
}

CalVector scaled(const CalVector& v, const CalVector& s) {
    return CalVector(v.x * s.x, v.y * s.y, v.z * s.z);
}

CalMatrix diag(const CalVector& s) {
    return CalMatrix(CalVector(s.x, 0, 0), CalVector(0, s.y, 0), CalVector(0, 0, s.z));
}

CalVector transformPointOf(const BoneTransform& bt, const CalVector& p) {
    return CalVector(
        bt.rowx.x * p.x + bt.rowx.y * p.y + bt.rowx.z * p.z + bt.rowx.w,
        bt.rowy.x * p.x + bt.rowy.y * p.y + bt.rowy.z * p.z + bt.rowy.w,
        bt.rowz.x * p.x + bt.rowz.y * p.y + bt.rowz.z * p.z + bt.rowz.w);
}

// Independent skinning reference for a single bone:
//   world = absBasis * (bindRotation * p + bindTranslation) + absTranslation
CalVector referenceSkinnedPoint(
    const CalMatrix& absBasis,
    const CalVector& absTranslation,
    const CalQuaternion& bindRotation,
    const CalVector& bindTranslation,
    const CalVector& p) {
    return absBasis * (bindRotation * p + bindTranslation) + absTranslation;
}

CalVector referenceSkinnedVector(
    const CalMatrix& absBasis,
    const CalQuaternion& bindRotation,
    const CalVector& n) {
    return absBasis * (bindRotation * n);
}

CalCoreTrack::KeyframeList keys2(float t0, const CalVector& s0, float t1, const CalVector& s1) {
    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(t0, CalVector(), CalQuaternion(), s0));
    keys.push_back(CalCoreKeyframe(t1, CalVector(), CalQuaternion(), s1));
    return keys;
}

// Two keyframes with the given translations and identity scale.
CalCoreTrack::KeyframeList translationKeys2(
    float t0, const CalVector& tr0, float t1, const CalVector& tr1) {
    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(t0, tr0, CalQuaternion()));
    keys.push_back(CalCoreKeyframe(t1, tr1, CalQuaternion()));
    return keys;
}

CalCoreTrack::KeyframeList keysWithTimesAndScales(const float* times, const CalVector* scales, size_t count) {
    CalCoreTrack::KeyframeList keys;
    for (size_t i = 0; i < count; ++i) {
        keys.push_back(CalCoreKeyframe(times[i], CalVector(), CalQuaternion(), scales[i]));
    }
    return keys;
}

ISTD::shared_ptr<CalCoreAnimation> wrapTrack(const CalCoreTrack& track) {
    ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
    animation->duration = 1.0f;
    animation->tracks.push_back(track);
    return animation;
}

// Priority-mixing reference, written from the task rules rather than reusing
// the mixer: a starts at 1; animations are visited in mixer order; an
// animation with a track for the bone contributes weight * ramp * a to the
// arithmetic mean and then, if its priority is nonzero, scales a by
// (1 - ramp).  An animation without a track leaves a untouched.
struct ScaleAnimationSpec {
    float weight;
    float ramp;
    unsigned priority;
    bool hasTrack;
    CalVector scale;
};

CalVector referenceMixedScale(const std::vector<ScaleAnimationSpec>& animations) {
    float a = 1.0f;
    float total = 0.0f;
    CalVector mean(1, 1, 1);

    for (size_t i = 0; i < animations.size(); ++i) {
        const ScaleAnimationSpec& spec = animations[i];
        if (!spec.hasTrack) {
            continue;
        }

        float w = spec.weight * spec.ramp * a;
        if (w != 0.0f) {
            total += w;
            float f = w / total;
            mean = lerp(f, mean, spec.scale);
        }

        if (spec.priority != 0) {
            a *= (1.0f - spec.ramp);
        }
    }

    return total != 0.0f ? mean : CalVector(1, 1, 1);
}

ISTD::shared_ptr<CalCoreAnimation> constantAnimation(
    const std::vector<std::pair<unsigned, CalVector> >& trackScales,
    const cal3d::RotateTranslate& pose = cal3d::RotateTranslate()) {
    ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
    animation->duration = 1.0f;
    for (size_t i = 0; i < trackScales.size(); ++i) {
        CalCoreTrack::KeyframeList keys;
        keys.push_back(CalCoreKeyframe(0.0f, pose.translation, pose.rotation, trackScales[i].second));
        animation->tracks.push_back(CalCoreTrack(trackScales[i].first, keys));
    }
    return animation;
}

ISTD::shared_ptr<CalAnimation> animationFor(
    const ISTD::shared_ptr<CalCoreAnimation>& core,
    float weight,
    unsigned priority,
    float ramp = 1.0f,
    float time = 0.0f) {
    ISTD::shared_ptr<CalAnimation> animation = ISTD::make_shared<CalAnimation>(core, weight, priority);
    animation->rampValue = ramp;
    animation->time = time;
    return animation;
}

// Three identity-rotation bones in a small hierarchy: 0 is the root, 1 and 2
// are its children with non-zero relative translations.
ISTD::shared_ptr<CalCoreSkeleton> prioritySkeleton() {
    ISTD::shared_ptr<CalCoreBone> root = ISTD::make_shared<CalCoreBone>("root", -1);
    ISTD::shared_ptr<CalCoreBone> left = ISTD::make_shared<CalCoreBone>("left", 0);
    left->relativeTransform.translation = CalVector(1, 2, 3);
    ISTD::shared_ptr<CalCoreBone> right = ISTD::make_shared<CalCoreBone>("right", 0);
    right->relativeTransform.translation = CalVector(4, 5, 6);

    ISTD::shared_ptr<CalCoreSkeleton> skeleton = ISTD::make_shared<CalCoreSkeleton>();
    skeleton->addCoreBone(root);
    skeleton->addCoreBone(left);
    skeleton->addCoreBone(right);
    return skeleton;
}

// Rotated parent/child with non-unit inverse bind poses.
struct SkinnedRig {
    ISTD::shared_ptr<CalCoreSkeleton> skeleton;
    CalQuaternion rootBindRotation;
    CalVector rootBindTranslation;
    CalQuaternion childBindRotation;
    CalVector childBindTranslation;
    CalQuaternion childRelativeRotation;
    CalVector childRelativeTranslation;
};

SkinnedRig makeSkinnedRig() {
    SkinnedRig rig;
    rig.rootBindRotation = axisAngle(CalVector(1, 0, 0), 1.5707963267948966f);
    rig.rootBindTranslation = CalVector(1, 0, 2);
    rig.childBindRotation = axisAngle(CalVector(0, 0, 1), 3.14159265358979323846f);
    rig.childBindTranslation = CalVector(0, 3, 0);
    rig.childRelativeRotation = axisAngle(CalVector(0, 0, 1), 1.5707963267948966f);
    rig.childRelativeTranslation = CalVector(2, 0, 0);

    ISTD::shared_ptr<CalCoreBone> root = ISTD::make_shared<CalCoreBone>("root", -1);
    root->inverseBindPoseTransform = cal3d::RotateTranslate(rig.rootBindRotation, rig.rootBindTranslation);

    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform = cal3d::RotateTranslate(rig.childRelativeRotation, rig.childRelativeTranslation);
    child->inverseBindPoseTransform = cal3d::RotateTranslate(rig.childBindRotation, rig.childBindTranslation);

    rig.skeleton = ISTD::make_shared<CalCoreSkeleton>();
    rig.skeleton->addCoreBone(root);
    rig.skeleton->addCoreBone(child);
    return rig;
}

// Animation for the rig whose pose matches the bind relative pose and whose
// keyframe scales are constant, so the expected output is fully closed-form.
ISTD::shared_ptr<CalCoreAnimation> rigAnimation(const SkinnedRig& rig, const CalVector& rootScale, const CalVector& childScale) {
    ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
    animation->duration = 1.0f;

    CalCoreTrack::KeyframeList rootKeys;
    rootKeys.push_back(CalCoreKeyframe(0.0f, CalVector(), CalQuaternion(), rootScale));
    animation->tracks.push_back(CalCoreTrack(0, rootKeys));

    CalCoreTrack::KeyframeList childKeys;
    childKeys.push_back(CalCoreKeyframe(
        0.0f,
        rig.childRelativeTranslation,
        rig.childRelativeRotation,
        childScale));
    animation->tracks.push_back(CalCoreTrack(1, childKeys));

    return animation;
}

void skinOne(
    const CalSkeleton& skeleton,
    const CalCoreSubmesh::Vertex& vertex,
    const CalCoreSubmesh::Influence* influences,
    CalVector4* output) {
    CalPhysique::calculateVerticesAndNormals_x87(
        &skeleton.boneTransforms[0],
        1,
        &vertex,
        influences,
        output);
}

int expectVector4(const CalVector4& actual, const CalVector& expected, float tol = 1e-4f) {
    TASK_CHECK(near(actual.x, expected.x, tol));
    TASK_CHECK(near(actual.y, expected.y, tol));
    TASK_CHECK(near(actual.z, expected.z, tol));
    return 0;
}

// ---------------------------------------------------------------------------
// sampling_assets
// ---------------------------------------------------------------------------

int case_scale_sampling_matches_reference() {
    const float times[] = {0.0f, 2.0f, 5.0f};
    const CalVector scales[] = {
        CalVector(2.0f, 1.0f, 0.5f),
        CalVector(4.0f, 2.0f, 1.5f),
        CalVector(1.0f, 3.0f, 1.0f)
    };
    CalCoreTrack track(0, keysWithTimesAndScales(times, scales, 3));

    TASK_CHECK(vecNear(track.getCurrentScale(-10.0f), CalVector(2.0f, 1.0f, 0.5f)));
    TASK_CHECK(vecNear(track.getCurrentScale(0.0f), CalVector(2.0f, 1.0f, 0.5f)));
    TASK_CHECK(vecNear(track.getCurrentScale(1.0f), CalVector(3.0f, 1.5f, 1.0f)));
    TASK_CHECK(vecNear(track.getCurrentScale(2.0f), CalVector(4.0f, 2.0f, 1.5f)));
    TASK_CHECK(vecNear(track.getCurrentScale(3.5f), CalVector(2.5f, 2.5f, 1.25f)));
    TASK_CHECK(vecNear(track.getCurrentScale(5.0f), CalVector(1.0f, 3.0f, 1.0f)));
    TASK_CHECK(vecNear(track.getCurrentScale(100.0f), CalVector(1.0f, 3.0f, 1.0f)));

    // Repeated sampling of the same instant must be stable and idempotent.
    CalVector first = track.getCurrentScale(3.5f);
    CalVector second = track.getCurrentScale(3.5f);
    TASK_CHECK(vecNear(first, second));

    // The scale channel is independent of the pose channel.
    TASK_CHECK(near(track.getCurrentTransform(3.5f).translation.x, 0.0f));

    CalCoreTrack empty(0, CalCoreTrack::KeyframeList());
    TASK_CHECK(vecNear(empty.getCurrentScale(-1.0f), CalVector(1.0f, 1.0f, 1.0f)));
    TASK_CHECK(vecNear(empty.getCurrentScale(1.0f), CalVector(1.0f, 1.0f, 1.0f)));

    // A single keyframe clamps in both directions.
    CalCoreTrack::KeyframeList one;
    one.push_back(CalCoreKeyframe(7.0f, CalVector(), CalQuaternion(), CalVector(0.25f, 0.5f, 4.0f)));
    CalCoreTrack single(0, one);
    TASK_CHECK(vecNear(single.getCurrentScale(-5.0f), CalVector(0.25f, 0.5f, 4.0f)));
    TASK_CHECK(vecNear(single.getCurrentScale(50.0f), CalVector(0.25f, 0.5f, 4.0f)));

    // Coincident keyframe times do not divide by zero.
    CalCoreTrack::KeyframeList coincident;
    coincident.push_back(CalCoreKeyframe(1.0f, CalVector(), CalQuaternion(), CalVector(2.0f, 2.0f, 2.0f)));
    coincident.push_back(CalCoreKeyframe(1.0f, CalVector(), CalQuaternion(), CalVector(6.0f, 6.0f, 6.0f)));
    CalCoreTrack coincidentTrack(0, coincident);
    CalVector coincidentScale = coincidentTrack.getCurrentScale(1.0f);
    TASK_CHECK(std::isfinite(coincidentScale.x));
    TASK_CHECK(near(coincidentScale.x, 6.0f));

    return 0;
}

int case_optimize_preserves_scale_only_changes() {
    // Two keyframes with the same pose but different scale are not redundant.
    CalCoreTrack track(0, keys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(2, 2, 2)));
    track.optimize();
    TASK_CHECK(track.keyframes.size() == 2u);
    TASK_CHECK(vecNear(track.keyframes[1].localScale, CalVector(2, 2, 2)));

    // Identical pose and scale still collapse.
    CalCoreTrack redundant(0, keys2(0.0f, CalVector(3, 3, 3), 1.0f, CalVector(3, 3, 3)));
    redundant.optimize();
    TASK_CHECK(redundant.keyframes.size() == 1u);
    TASK_CHECK(vecNear(redundant.keyframes[0].localScale, CalVector(3, 3, 3)));

    // optimize() through CalCoreAnimation must keep a scale-only track intact.
    ISTD::shared_ptr<CalCoreAnimation> animation = wrapTrack(track);
    animation->optimize();
    TASK_CHECK(animation->tracks.size() == 1u);
    TASK_CHECK(animation->tracks[0].keyframes.size() == 2u);
    TASK_CHECK(vecNear(animation->tracks[0].getCurrentScale(0.5f), CalVector(1.5f, 1.5f, 1.5f)));

    return 0;
}

int case_compress_keeps_and_removes_scale_keys() {
    // Purely linear scale channel: interior keys are truly redundant and may
    // be deleted, and sampling at every original key time must be preserved.
    {
        const float times[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
        const CalVector scales[] = {
            CalVector(1, 1, 1), CalVector(2, 2, 2), CalVector(3, 3, 3),
            CalVector(4, 4, 4), CalVector(5, 5, 5)
        };
        CalCoreTrack track(0, keysWithTimesAndScales(times, scales, 5));
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.0, 0.0, 0);

        // The whole scale track must not be dropped.
        TASK_CHECK(compressed->keyframes.size() >= 2u);
        TASK_CHECK(compressed->keyframes.size() < 5u);
        TASK_CHECK(vecNear(compressed->keyframes.front().localScale, CalVector(1, 1, 1)));
        TASK_CHECK(vecNear(compressed->keyframes.back().localScale, CalVector(5, 5, 5)));

        for (size_t i = 0; i < 5; ++i) {
            TASK_CHECK(vecNear(compressed->getCurrentScale(times[i]), scales[i]));
        }
    }

    // Non-linear scale must be retained at the bump.
    {
        const float times[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
        const CalVector scales[] = {
            CalVector(1, 1, 1), CalVector(1, 1, 1), CalVector(1, 1, 1),
            CalVector(5, 5, 5), CalVector(1, 1, 1)
        };
        CalCoreTrack track(0, keysWithTimesAndScales(times, scales, 5));
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.0, 0.0, 0);

        TASK_CHECK(compressed->keyframes.size() >= 2u);
        bool keptBump = false;
        for (size_t i = 0; i < compressed->keyframes.size(); ++i) {
            if (near(compressed->keyframes[i].time, 3.0f) &&
                vecNear(compressed->keyframes[i].localScale, CalVector(5, 5, 5))) {
                keptBump = true;
            }
        }
        TASK_CHECK(keptBump);

        // Every original key time still samples within tolerance.
        for (size_t i = 0; i < 5; ++i) {
            TASK_CHECK(vecNear(compressed->getCurrentScale(times[i]), scales[i]));
        }
    }

    // A scale deviation larger than 1e-5 is not redundant.
    {
        const float times[] = {0.0f, 1.0f, 2.0f};
        const CalVector scales[] = {
            CalVector(1, 1, 1),
            CalVector(2.0f, 2.0f, 2.0f + 2e-5f),
            CalVector(3, 3, 3)
        };
        CalCoreTrack track(0, keysWithTimesAndScales(times, scales, 3));
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.0, 0.0, 0);
        TASK_CHECK(compressed->keyframes.size() == 3u);
        TASK_CHECK(vecNear(compressed->getCurrentScale(1.0f), scales[1]));
    }

    // A 3-key bend in one component blocks elimination even though the other
    // two components are linear.
    {
        const float times[] = {0.0f, 1.0f, 2.0f};
        const CalVector scales[] = {
            CalVector(1, 1, 1), CalVector(2, 5, 2), CalVector(3, 1, 3)
        };
        CalCoreTrack track(0, keysWithTimesAndScales(times, scales, 3));
        ISTD::shared_ptr<CalCoreTrack> compressed = track.compress(0.0, 0.0, 0);
        TASK_CHECK(compressed->keyframes.size() == 3u);
    }

    return 0;
}

int case_xml_scale_roundtrip_and_validation() {
    // Non-identity scale must survive an XML write/read round trip.
    {
        ISTD::shared_ptr<CalCoreAnimation> animation = wrapTrack(CalCoreTrack(0,
            keys2(0.0f, CalVector(2, 3, 4), 1.0f, CalVector(0.5f, 1.5f, 2.5f))));

        std::string xml = CalSaver::saveCoreAnimationXmlToBuffer(animation);
        TASK_CHECK(!xml.empty());
        TASK_CHECK(xml.find("SCALE") != std::string::npos);

        CalBufferSource source(xml.data(), xml.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(loaded);
        TASK_CHECK(loaded->tracks.size() == 1u);
        TASK_CHECK(loaded->tracks[0].keyframes.size() == 2u);
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[0].localScale, CalVector(2, 3, 4)));
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[1].localScale, CalVector(0.5f, 1.5f, 2.5f)));
        TASK_CHECK(vecNear(loaded->tracks[0].getCurrentScale(0.5f), CalVector(1.25f, 2.25f, 3.25f)));

        // Re-saving the loaded animation reproduces the same XML.
        std::string again = CalSaver::saveCoreAnimationXmlToBuffer(loaded);
        TASK_CHECK(again == xml);
    }

    // Legacy XML without SCALE keeps loading and re-saves byte-for-byte with
    // identity scale.
    const std::string legacy =
        "<HEADER MAGIC=\"XAF\" VERSION=\"919\" />\n"
        "<ANIMATION NUMTRACKS=\"1\" DURATION=\"40\">\n"
        "    <TRACK BONEID=\"0\" TRANSLATIONREQUIRED=\"0\" TRANSLATIONISDYNAMIC=\"0\" HIGHRANGEREQUIRED=\"1\" NUMKEYFRAMES=\"2\">\n"
        "        <KEYFRAME TIME=\"0\">\n"
        "            <ROTATION>0.5 0.5 0.5 -0.5</ROTATION>\n"
        "        </KEYFRAME>\n"
        "        <KEYFRAME TIME=\"40\">\n"
        "            <ROTATION>0.5 0.5 0.5 -0.5</ROTATION>\n"
        "        </KEYFRAME>\n"
        "    </TRACK>\n"
        "</ANIMATION>\n";
    {
        CalBufferSource source(legacy.data(), legacy.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(loaded);
        TASK_CHECK(loaded->tracks.size() == 1u);
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[0].localScale, CalVector(1, 1, 1)));
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[1].localScale, CalVector(1, 1, 1)));
        TASK_CHECK(CalSaver::saveCoreAnimationXmlToBuffer(loaded) == legacy);
    }

    // SCALE is written after ROTATION and omitted for identity.
    {
        std::string xml = CalSaver::saveCoreAnimationXmlToBuffer(
            wrapTrack(CalCoreTrack(0, translationKeys2(0.0f, CalVector(0.25f, 0.5f, 0.75f), 1.0f, CalVector(1, 2, 3)))));
        TASK_CHECK(!xml.empty());
        TASK_CHECK(xml.find("<SCALE>") == std::string::npos);

        std::string scaledXml = CalSaver::saveCoreAnimationXmlToBuffer(
            wrapTrack(CalCoreTrack(0, keys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(2, 3, 4)))));
        size_t rotationPos = scaledXml.find("<ROTATION>");
        size_t scalePos = scaledXml.find("<SCALE>");
        TASK_CHECK(rotationPos != std::string::npos);
        TASK_CHECK(scalePos != std::string::npos);
        TASK_CHECK(rotationPos < scalePos);
    }

    // Explicitly supplied SCALE is read.
    {
        const std::string scaledXml =
            "<HEADER MAGIC=\"XAF\" VERSION=\"919\" />\n"
            "<ANIMATION NUMTRACKS=\"1\" DURATION=\"1\">\n"
            "    <TRACK BONEID=\"0\" TRANSLATIONREQUIRED=\"0\" TRANSLATIONISDYNAMIC=\"0\" HIGHRANGEREQUIRED=\"1\" NUMKEYFRAMES=\"1\">\n"
            "        <KEYFRAME TIME=\"0\">\n"
            "            <ROTATION>0 0 0 1</ROTATION>\n"
            "            <SCALE>2 3 4</SCALE>\n"
            "        </KEYFRAME>\n"
            "    </TRACK>\n"
            "</ANIMATION>\n";
        CalBufferSource source(scaledXml.data(), scaledXml.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(loaded);
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[0].localScale, CalVector(2, 3, 4)));
    }

    // Invalid scale payloads must fail loading.
    const char* invalidScales[] = {
        "<SCALE>1 2</SCALE>",          // missing component
        "<SCALE>1 2 3 4</SCALE>",      // extra component
        "<SCALE>1 zero 3</SCALE>",     // non-numeric
        "<SCALE>1 inf 3</SCALE>",      // non-finite
        "<SCALE>1 nan 3</SCALE>",      // non-finite
        "<SCALE>1 0 3</SCALE>",        // non-positive
        "<SCALE>1 -2 3</SCALE>",       // non-positive
        "<SCALE></SCALE>"              // empty
    };
    for (size_t i = 0; i < sizeof(invalidScales) / sizeof(invalidScales[0]); ++i) {
        std::string xml =
            "<HEADER MAGIC=\"XAF\" VERSION=\"919\" />\n"
            "<ANIMATION NUMTRACKS=\"1\" DURATION=\"1\">\n"
            "    <TRACK BONEID=\"0\" TRANSLATIONREQUIRED=\"0\" TRANSLATIONISDYNAMIC=\"0\" HIGHRANGEREQUIRED=\"1\" NUMKEYFRAMES=\"1\">\n"
            "        <KEYFRAME TIME=\"0\">\n"
            "            <ROTATION>0 0 0 1</ROTATION>\n"
            "            ";
        xml += invalidScales[i];
        xml += "\n        </KEYFRAME>\n    </TRACK>\n</ANIMATION>\n";

        CalBufferSource source(xml.data(), xml.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(!loaded);
    }

    // A misplaced SCALE (before ROTATION) is invalid too.
    {
        const std::string xml =
            "<HEADER MAGIC=\"XAF\" VERSION=\"919\" />\n"
            "<ANIMATION NUMTRACKS=\"1\" DURATION=\"1\">\n"
            "    <TRACK BONEID=\"0\" TRANSLATIONREQUIRED=\"0\" TRANSLATIONISDYNAMIC=\"0\" HIGHRANGEREQUIRED=\"1\" NUMKEYFRAMES=\"1\">\n"
            "        <KEYFRAME TIME=\"0\">\n"
            "            <SCALE>2 3 4</SCALE>\n"
            "            <ROTATION>0 0 0 1</ROTATION>\n"
            "        </KEYFRAME>\n"
            "    </TRACK>\n"
            "</ANIMATION>\n";
        CalBufferSource source(xml.data(), xml.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(!loaded);
    }

    return 0;
}

int case_binary_save_rejects_animated_scale() {
    // The CAF format has no scale channel: refuse rather than silently drop it.
    {
        ISTD::shared_ptr<CalCoreAnimation> animation = wrapTrack(CalCoreTrack(0,
            keys2(0.0f, CalVector(1, 2, 3), 1.0f, CalVector(2, 2, 2))));

        CalError::setLastError(CalError::OK, __FILE__, __LINE__);
        std::string binary = CalSaver::saveCoreAnimationToBuffer(animation);
        TASK_CHECK(binary.empty());
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_ATTRIBUTE_VALUE);

        std::ostringstream stream;
        TASK_CHECK(!CalSaver::saveCoreAnimation(stream, animation.get()));
        TASK_CHECK(stream.str().empty());

        // XML can still carry it.
        TASK_CHECK(!CalSaver::saveCoreAnimationXmlToBuffer(animation).empty());
    }

    // One offending track anywhere in the animation is enough.
    {
        ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
        animation->duration = 1.0f;
        animation->tracks.push_back(CalCoreTrack(0, translationKeys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(1, 1, 1))));
        animation->tracks.push_back(CalCoreTrack(1, keys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(1, 2, 1))));

        std::ostringstream stream;
        TASK_CHECK(!CalSaver::saveCoreAnimation(stream, animation.get()));
        TASK_CHECK(stream.str().empty());
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_ATTRIBUTE_VALUE);
    }

    // Identity scale saves and loads as a normal CAF, defaulting to identity.
    {
        ISTD::shared_ptr<CalCoreAnimation> animation = wrapTrack(CalCoreTrack(0,
            translationKeys2(0.0f, CalVector(1, 2, 3), 1.0f, CalVector(4, 5, 6))));

        std::string binary = CalSaver::saveCoreAnimationToBuffer(animation);
        TASK_CHECK(!binary.empty());

        CalBufferSource source(binary.data(), binary.size());
        ISTD::shared_ptr<CalCoreAnimation> loaded = CalLoader::loadCoreAnimation(source);
        TASK_CHECK(loaded);
        TASK_CHECK(loaded->tracks.size() == 1u);
        TASK_CHECK(loaded->tracks[0].keyframes.size() == 2u);
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[0].localScale, CalVector(1, 1, 1)));
        TASK_CHECK(vecNear(loaded->tracks[0].keyframes[1].localScale, CalVector(1, 1, 1)));
        TASK_CHECK(near(loaded->tracks[0].keyframes[0].transform.translation.x, 1.0f));
    }

    return 0;
}

int case_scale_method_is_unit_conversion_only() {
    // CalCoreKeyframe::scale is a model-unit conversion: translation only.
    CalCoreKeyframe keyframe(0.0f, CalVector(1, 2, 3), CalQuaternion(), CalVector(2, 3, 4));
    keyframe.scale(10.0f);
    TASK_CHECK(vecNear(keyframe.transform.translation, CalVector(10, 20, 30)));
    TASK_CHECK(vecNear(keyframe.localScale, CalVector(2, 3, 4)));

    // The invalid-translation sentinel is still left alone.
    CalCoreKeyframe sentinel(0.0f, InvalidTranslation, CalQuaternion(), CalVector(2, 3, 4));
    sentinel.scale(10.0f);
    TASK_CHECK(vecNear(sentinel.transform.translation, InvalidTranslation));
    TASK_CHECK(vecNear(sentinel.localScale, CalVector(2, 3, 4)));

    // Track and animation scaling follow the same rule.
    CalCoreTrack track(0, translationKeys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(5, 5, 5)));
    track.scale(2.0f);
    TASK_CHECK(vecNear(track.keyframes[0].transform.translation, CalVector(2, 2, 2)));
    TASK_CHECK(vecNear(track.keyframes[1].transform.translation, CalVector(10, 10, 10)));
    TASK_CHECK(vecNear(track.keyframes[0].localScale, CalVector(1, 1, 1)));
    TASK_CHECK(vecNear(track.keyframes[1].localScale, CalVector(1, 1, 1)));

    ISTD::shared_ptr<CalCoreAnimation> animation = wrapTrack(track);
    animation->scale(0.5f);
    TASK_CHECK(vecNear(animation->tracks[0].keyframes[0].transform.translation, CalVector(1, 1, 1)));
    TASK_CHECK(vecNear(animation->tracks[0].keyframes[1].localScale, CalVector(1, 1, 1)));

    return 0;
}

// ---------------------------------------------------------------------------
// priority_frames
// ---------------------------------------------------------------------------

int case_priority_mixing_matches_reference() {
    ISTD::shared_ptr<CalCoreSkeleton> core = prioritySkeleton();
    CalSkeleton skeleton(core);
    CalMixer mixer;

    ISTD::shared_ptr<CalCoreAnimation> lowCore = constantAnimation(
        std::vector<std::pair<unsigned, CalVector> >{
            std::make_pair(0u, CalVector(2, 2, 2)),
            std::make_pair(1u, CalVector(4, 4, 4))});
    ISTD::shared_ptr<CalCoreAnimation> highCore = constantAnimation(
        std::vector<std::pair<unsigned, CalVector> >{
            std::make_pair(0u, CalVector(10, 10, 10))});

    ISTD::shared_ptr<CalAnimation> low = animationFor(lowCore, 1.0f, 0, 1.0f, 0.0f);
    ISTD::shared_ptr<CalAnimation> high = animationFor(highCore, 1.0f, 1, 0.5f, 0.0f);

    mixer.addAnimation(low);
    mixer.addAnimation(high);
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());

    // Bone 0: high-priority animation runs first with effective weight 0.5,
    // then attenuates the low-priority weight by half.
    std::vector<ScaleAnimationSpec> bone0;
    bone0.push_back(ScaleAnimationSpec{1.0f, 0.5f, 1u, true, CalVector(10, 10, 10)});
    bone0.push_back(ScaleAnimationSpec{1.0f, 1.0f, 0u, true, CalVector(2, 2, 2)});
    TASK_CHECK(vecNear(skeleton.bones[0].scale.scale, referenceMixedScale(bone0)));

    // Bone 1: the high-priority animation has no track, so it must not consume
    // the remaining coefficient.
    std::vector<ScaleAnimationSpec> bone1;
    bone1.push_back(ScaleAnimationSpec{1.0f, 0.5f, 1u, false, CalVector()});
    bone1.push_back(ScaleAnimationSpec{1.0f, 1.0f, 0u, true, CalVector(4, 4, 4)});
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, referenceMixedScale(bone1)));
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(4, 4, 4)));

    // Bone 2 has no tracks at all and stays at identity.
    TASK_CHECK(vecNear(skeleton.bones[2].scale.scale, CalVector(1, 1, 1)));

    return 0;
}

int case_weighted_mix_and_ramp() {
    ISTD::shared_ptr<CalCoreSkeleton> core = prioritySkeleton();

    // Two additive low-priority animations: weighted arithmetic mean.
    {
        CalSkeleton skeleton(core);
        CalMixer mixer;
        ISTD::shared_ptr<CalCoreAnimation> aCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(1, 1, 1))});
        ISTD::shared_ptr<CalCoreAnimation> bCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(5, 5, 5))});
        mixer.addAnimation(animationFor(aCore, 1.0f, 0, 1.0f, 0.0f));
        mixer.addAnimation(animationFor(bCore, 3.0f, 0, 1.0f, 0.0f));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
        TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(4, 4, 4)));
    }

    // A fully ramped-in nonzero-priority animation annihilates later weights.
    {
        CalSkeleton skeleton(core);
        CalMixer mixer;
        ISTD::shared_ptr<CalCoreAnimation> topCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(7, 7, 7))});
        ISTD::shared_ptr<CalCoreAnimation> bottomCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(2, 2, 2))});
        mixer.addAnimation(animationFor(topCore, 1.0f, 5u, 1.0f, 0.0f));
        mixer.addAnimation(animationFor(bottomCore, 1.0f, 0u, 1.0f, 0.0f));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
        TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(7, 7, 7)));
    }

    // A zero-weight contributor adds nothing to the mean, but if it has a
    // track and nonzero priority it still consumes the remaining coefficient;
    // with every later contribution zeroed out the result is identity.
    {
        CalSkeleton skeleton(core);
        CalMixer mixer;
        ISTD::shared_ptr<CalCoreAnimation> zeroCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(100, 100, 100))});
        ISTD::shared_ptr<CalCoreAnimation> bottomCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(2, 2, 2))});
        mixer.addAnimation(animationFor(zeroCore, 0.0f, 3u, 1.0f, 0.0f));
        mixer.addAnimation(animationFor(bottomCore, 1.0f, 0u, 1.0f, 0.0f));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
        TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(1, 1, 1)));
    }

    // The same zero-weight animation at priority 0 consumes nothing, so the
    // later animation still contributes in full.
    {
        CalSkeleton skeleton(core);
        CalMixer mixer;
        ISTD::shared_ptr<CalCoreAnimation> zeroCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(100, 100, 100))});
        ISTD::shared_ptr<CalCoreAnimation> bottomCore = constantAnimation(
            std::vector<std::pair<unsigned, CalVector> >{std::make_pair(1u, CalVector(2, 2, 2))});
        mixer.addAnimation(animationFor(zeroCore, 0.0f, 0u, 1.0f, 0.0f));
        mixer.addAnimation(animationFor(bottomCore, 1.0f, 0u, 1.0f, 0.0f));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
        TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(2, 2, 2)));
    }

    return 0;
}

int case_explicit_adjustments_compose() {
    ISTD::shared_ptr<CalCoreSkeleton> core = prioritySkeleton();

    // BoneScaleAdjustment's last value multiplies the animation scale per
    // component; bone 0 has identity bind pose so the absolute matrix is
    // exactly diag(adjustment * animation scale).
    CalSkeleton skeleton(core);
    CalMixer mixer;
    ISTD::shared_ptr<CalCoreAnimation> coreAnimation = constantAnimation(
        std::vector<std::pair<unsigned, CalVector> >{std::make_pair(0u, CalVector(2, 2, 2))});
    mixer.addAnimation(animationFor(coreAnimation, 1.0f, 0u, 1.0f, 0.0f));

    std::vector<BoneScaleAdjustment> scaleAdjustments;
    scaleAdjustments.push_back(BoneScaleAdjustment(0, CalVector(9, 9, 9)));
    scaleAdjustments.push_back(BoneScaleAdjustment(0, CalVector(0.5f, 1.0f, 4.0f)));
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), scaleAdjustments);

    TASK_CHECK(vecNear(skeleton.bones[0].scale.scale, CalVector(2, 2, 2)));
    TASK_CHECK(vecNear(transformPointOf(skeleton.boneTransforms[0], CalVector(1, 1, 1)),
                       CalVector(1.0f, 2.0f, 8.0f)));

    // A transform adjustment only replaces the pose; it must not attenuate the
    // animation's scale weights.
    CalSkeleton adjusted(core);
    CalMixer adjustedMixer;
    adjustedMixer.addAnimation(animationFor(coreAnimation, 1.0f, 0u, 1.0f, 0.0f));

    std::vector<BoneTransformAdjustment> transformAdjustments;
    BoneTransformAdjustment poseAdjustment;
    poseAdjustment.boneId = 0;
    poseAdjustment.localOri = axisAngle(CalVector(0, 0, 1), 1.5707963267948966f);
    poseAdjustment.rampValue = 1.0f;
    transformAdjustments.push_back(poseAdjustment);

    std::vector<BoneScaleAdjustment> noScaleAdjustments;
    adjustedMixer.updateSkeleton(&adjusted, transformAdjustments, noScaleAdjustments);

    TASK_CHECK(vecNear(adjusted.bones[0].scale.scale, CalVector(2, 2, 2)));
    TASK_CHECK(vecNear(adjusted.bones[0].absoluteTransform.translation, CalVector()));
    // The replace pose is applied, then the animation scale (2,2,2) is applied
    // in bone space, so the absolute basis is R * diag(2,2,2).
    CalVector rotated = adjusted.bones[0].absoluteTransform.basis * CalVector(1, 0, 0);
    TASK_CHECK(vecNear(rotated, poseAdjustment.localOri * CalVector(2, 0, 0)));
    TASK_CHECK(near(rotated.length(), 2.0f));

    return 0;
}

int case_removal_and_cross_skeleton_reset() {
    ISTD::shared_ptr<CalCoreSkeleton> core = prioritySkeleton();
    CalSkeleton skeleton(core);
    CalSkeleton other(core);

    CalMixer mixer;
    CalMixer otherMixer;

    ISTD::shared_ptr<CalCoreAnimation> coreAnimation = constantAnimation(
        std::vector<std::pair<unsigned, CalVector> >{
            std::make_pair(1u, CalVector(3, 3, 3)),
            std::make_pair(2u, CalVector(5, 5, 5))});
    ISTD::shared_ptr<CalAnimation> animation = animationFor(coreAnimation, 1.0f, 0u, 1.0f, 0.0f);
    mixer.addAnimation(animation);

    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(3, 3, 3)));
    TASK_CHECK(vecNear(skeleton.bones[2].scale.scale, CalVector(5, 5, 5)));
    CalVector firstPose = skeleton.bones[1].absoluteTransform.translation;

    // Re-running the same instant must not accumulate scale.
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(3, 3, 3)));
    TASK_CHECK(vecNear(skeleton.bones[1].absoluteTransform.translation, firstPose));

    // Updating another character must not inherit this character's scale.
    otherMixer.updateSkeleton(&other, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(other.bones[1].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(vecNear(other.bones[2].scale.scale, CalVector(1, 1, 1)));

    // Removing the animation restores identity scale and the bind pose.
    mixer.removeAnimation(animation);
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(vecNear(skeleton.bones[2].scale.scale, CalVector(1, 1, 1)));
    TASK_CHECK(vecNear(skeleton.bones[1].absoluteTransform.translation, CalVector(1, 2, 3)));

    // Scale is sampled per frame: advancing the animation updates it.
    ISTD::shared_ptr<CalCoreAnimation> varying = ISTD::make_shared<CalCoreAnimation>();
    varying->duration = 1.0f;
    varying->tracks.push_back(CalCoreTrack(1, keys2(0.0f, CalVector(1, 1, 1), 1.0f, CalVector(3, 3, 3))));
    ISTD::shared_ptr<CalAnimation> varyingAnimation = animationFor(varying, 1.0f, 0u, 1.0f, 0.0f);
    mixer.addAnimation(varyingAnimation);
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(1, 1, 1)));
    varyingAnimation->time = 0.5f;
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(2, 2, 2)));
    mixer.removeAnimation(varyingAnimation);
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, CalVector(1, 1, 1)));

    return 0;
}

// ---------------------------------------------------------------------------
// skinning
// ---------------------------------------------------------------------------

int case_mixer_skeleton_physique_with_animated_scale() {
    SkinnedRig rig = makeSkinnedRig();
    CalSkeleton skeleton(rig.skeleton);
    CalMixer mixer;

    const CalVector rootScale(2, 1, 1);
    const CalVector childScale(1, 2, 1);
    ISTD::shared_ptr<CalCoreAnimation> coreAnimation = rigAnimation(rig, rootScale, childScale);
    mixer.addAnimation(animationFor(coreAnimation, 1.0f, 0u, 1.0f, 0.0f));
    mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());

    TASK_CHECK(vecNear(skeleton.bones[0].scale.scale, rootScale));
    TASK_CHECK(vecNear(skeleton.bones[1].scale.scale, childScale));

    // Absolute child translation is the parent scale applied to the relative
    // translation; the pose itself contributes no additional motion.
    TASK_CHECK(vecNear(skeleton.bones[0].absoluteTransform.translation, CalVector()));
    TASK_CHECK(vecNear(skeleton.bones[1].absoluteTransform.translation,
                       scaled(rig.childRelativeTranslation, rootScale)));

    const CalCoreSubmesh::Influence influences[] = {
        CalCoreSubmesh::Influence(0, 0.25f, false),
        CalCoreSubmesh::Influence(1, 0.75f, true)
    };
    const CalCoreSubmesh::Vertex vertex = {CalPoint4(1, 2, 3), CalVector4(1, 0, 0)};
    CalVector4 output[2];
    skinOne(skeleton, vertex, influences, output);

    // Expected per-bone transforms, derived from the propagation rules:
    //   root : absBasis = diag(rootScale), bind = root inverse bind
    //   child: absBasis = Rrel * diag(rootScale * childScale)
    //          absTranslation = rootScale * childRelativeTranslation
    CalVector childAbsoluteScale = scaled(rootScale, childScale);

    CalVector rootPoint = referenceSkinnedPoint(
        diag(rootScale),
        CalVector(),
        rig.rootBindRotation,
        rig.rootBindTranslation,
        CalVector(1, 2, 3));
    CalVector rootNormal = referenceSkinnedVector(
        diag(rootScale),
        rig.rootBindRotation,
        CalVector(1, 0, 0));

    CalMatrix childBasis(rig.childRelativeRotation);
    childBasis = childBasis * diag(childAbsoluteScale);
    CalVector childPoint = referenceSkinnedPoint(
        childBasis,
        scaled(rig.childRelativeTranslation, rootScale),
        rig.childBindRotation,
        rig.childBindTranslation,
        CalVector(1, 2, 3));
    CalVector childNormal = referenceSkinnedVector(
        childBasis,
        rig.childBindRotation,
        CalVector(1, 0, 0));

    CalVector expectedPoint = rootPoint * 0.25f + childPoint * 0.75f;
    CalVector expectedNormal = rootNormal * 0.25f + childNormal * 0.75f;

    TASK_CHECK(expectVector4(output[0], expectedPoint) == 0);
    TASK_CHECK(expectVector4(output[1], expectedNormal) == 0);

    // Concrete closed-form values, checked independently of the helper above.
    TASK_CHECK(near(output[0].x, 2.5f, 1e-4f));
    TASK_CHECK(near(output[0].y, -2.25f, 1e-4f));
    TASK_CHECK(near(output[0].z, 3.25f, 1e-4f));
    TASK_CHECK(near(output[1].x, 0.5f, 1e-4f));
    TASK_CHECK(near(output[1].y, -1.5f, 1e-4f));
    TASK_CHECK(near(output[1].z, 0.0f, 1e-4f));

    // Pose-only behaviour is unchanged: with identity scales the same rig
    // reproduces the pre-existing translation/rotation result.
    CalSkeleton identitySkeleton(rig.skeleton);
    CalMixer identityMixer;
    identityMixer.addAnimation(animationFor(
        rigAnimation(rig, CalVector(1, 1, 1), CalVector(1, 1, 1)),
        1.0f, 0u, 1.0f, 0.0f));
    identityMixer.updateSkeleton(&identitySkeleton, std::vector<BoneTransformAdjustment>(), std::vector<BoneScaleAdjustment>());

    CalVector4 identityOutput[2];
    skinOne(identitySkeleton, vertex, influences, identityOutput);

    CalMatrix identityChildBasis(rig.childRelativeRotation);
    CalVector identityRootPoint = referenceSkinnedPoint(
        CalMatrix(),
        CalVector(),
        rig.rootBindRotation,
        rig.rootBindTranslation,
        CalVector(1, 2, 3));
    CalVector identityChildPoint = referenceSkinnedPoint(
        identityChildBasis,
        rig.childRelativeTranslation,
        rig.childBindRotation,
        rig.childBindTranslation,
        CalVector(1, 2, 3));
    CalVector identityPoint = identityRootPoint * 0.25f + identityChildPoint * 0.75f;
    TASK_CHECK(expectVector4(identityOutput[0], identityPoint) == 0);
    TASK_CHECK(near(identityOutput[0].x, 1.25f, 1e-4f));
    TASK_CHECK(near(identityOutput[0].y, -1.5f, 1e-4f));
    TASK_CHECK(near(identityOutput[0].z, 3.25f, 1e-4f));

    return 0;
}

int case_bone_scale_adjustment_propagation() {
    SkinnedRig rig = makeSkinnedRig();
    const CalCoreSubmesh::Influence childOnly[] = {CalCoreSubmesh::Influence(1, 1.0f, true)};
    const CalCoreSubmesh::Vertex vertex = {CalPoint4(1, 2, 3), CalVector4(0, 0, 1)};

    // Parent adjustment only: propagated into the child's absolute pose.
    {
        CalSkeleton skeleton(rig.skeleton);
        CalMixer mixer;
        std::vector<BoneScaleAdjustment> adjustments;
        adjustments.push_back(BoneScaleAdjustment(0, CalVector(2, 2, 2)));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), adjustments);

        TASK_CHECK(vecNear(skeleton.bones[1].absoluteTransform.translation,
                           scaled(rig.childRelativeTranslation, CalVector(2, 2, 2))));

        CalVector4 output[2];
        skinOne(skeleton, vertex, childOnly, output);

        CalVector absoluteScale(2, 2, 2);
        CalMatrix basis(rig.childRelativeRotation);
        basis = basis * diag(absoluteScale);
        CalVector expected = referenceSkinnedPoint(
            basis,
            scaled(rig.childRelativeTranslation, absoluteScale),
            rig.childBindRotation,
            rig.childBindTranslation,
            CalVector(1, 2, 3));

        TASK_CHECK(expectVector4(output[0], expected) == 0);
        TASK_CHECK(near(output[0].x, 2.0f, 1e-4f));        TASK_CHECK(near(output[0].y, -2.0f, 1e-4f));
        TASK_CHECK(near(output[0].z, 6.0f, 1e-4f));
    }

    // Parent and child adjustments multiply per component.
    {
        CalSkeleton skeleton(rig.skeleton);
        CalMixer mixer;
        std::vector<BoneScaleAdjustment> adjustments;
        adjustments.push_back(BoneScaleAdjustment(0, CalVector(2, 2, 2)));
        adjustments.push_back(BoneScaleAdjustment(1, CalVector(1, 3, 1)));
        mixer.updateSkeleton(&skeleton, std::vector<BoneTransformAdjustment>(), adjustments);

        CalVector4 output[2];
        skinOne(skeleton, vertex, childOnly, output);

        CalVector composedScale(2, 6, 2);
        CalMatrix basis(rig.childRelativeRotation);
        basis = basis * diag(composedScale);
        CalVector expected = referenceSkinnedPoint(
            basis,
            scaled(rig.childRelativeTranslation, CalVector(2, 2, 2)),
            rig.childBindRotation,
            rig.childBindTranslation,
            CalVector(1, 2, 3));

        TASK_CHECK(expectVector4(output[0], expected) == 0);
        TASK_CHECK(near(output[0].x, -2.0f, 1e-4f));
        TASK_CHECK(near(output[0].y, -2.0f, 1e-4f));
        TASK_CHECK(near(output[0].z, 6.0f, 1e-4f));
    }

    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"sampling_assets", "scale_sampling_matches_reference", case_scale_sampling_matches_reference},
    {"sampling_assets", "optimize_preserves_scale_only_changes", case_optimize_preserves_scale_only_changes},
    {"sampling_assets", "compress_keeps_and_removes_scale_keys", case_compress_keeps_and_removes_scale_keys},
    {"sampling_assets", "xml_scale_roundtrip_and_validation", case_xml_scale_roundtrip_and_validation},
    {"sampling_assets", "binary_save_rejects_animated_scale", case_binary_save_rejects_animated_scale},
    {"sampling_assets", "scale_method_is_unit_conversion_only", case_scale_method_is_unit_conversion_only},

    {"priority_frames", "priority_mixing_matches_reference", case_priority_mixing_matches_reference},
    {"priority_frames", "weighted_mix_and_ramp", case_weighted_mix_and_ramp},
    {"priority_frames", "explicit_adjustments_compose", case_explicit_adjustments_compose},
    {"priority_frames", "removal_and_cross_skeleton_reset", case_removal_and_cross_skeleton_reset},

    {"skinning", "mixer_skeleton_physique_with_animated_scale", case_mixer_skeleton_physique_with_animated_scale},
    {"skinning", "bone_scale_adjustment_propagation", case_bone_scale_adjustment_propagation}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
