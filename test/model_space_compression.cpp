//****************************************************************************//
// model_space_compression.cpp                                                //
// Devcheck for CalCoreAnimation::compressModelSpace.                         //
//                                                                            //
// Built and run through devcheck/run-current.sh: a standalone main, compiled //
// with the whole cal3d tree and the scalar (x87) skinning path.              //
//****************************************************************************//

#include <cal3d/animation.h>
#include <cal3d/bonetransform.h>
#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coretrack.h>
#include <cal3d/memory.h>
#include <cal3d/mixer.h>
#include <cal3d/modelspacecompression.h>
#include <cal3d/physique.h>
#include <cal3d/skeleton.h>
#include <cal3d/submesh.h>

#include <cassert>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    using cal3d::ModelSpaceProbe;
    using cal3d::ModelSpaceCompressionResult;

    typedef ISTD::shared_ptr<CalCoreSkeleton> CoreSkeletonPtr;
    typedef ISTD::shared_ptr<CalCoreAnimation> CoreAnimationPtr;
    typedef ISTD::shared_ptr<CalCoreBone> CoreBonePtr;

    void check(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "CHECK FAILED: " << message << std::endl;
            assert(false);
            std::abort();
        }
    }

    bool close(double a, double b, double tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    CalQuaternion rotationZ(float angle) {
        CalQuaternion q;
        q.setAxisAngle(CalVector(0, 0, 1), angle);
        return q;
    }

    CalQuaternion rotationY(float angle) {
        CalQuaternion q;
        q.setAxisAngle(CalVector(0, 1, 0), angle);
        return q;
    }

    CoreBonePtr makeBone(const std::string& name, int parent, const CalVector& translation) {
        CoreBonePtr bone = ISTD::make_shared<CalCoreBone>(name, parent);
        bone->relativeTransform.translation = translation;
        return bone;
    }

    CalCoreTrack makeTrack(unsigned boneId, const CalCoreTrack::KeyframeList& keyframes) {
        return CalCoreTrack(static_cast<int>(boneId), keyframes);
    }

    CalCoreKeyframe key(float time, const CalVector& translation, const CalQuaternion& rotation) {
        return CalCoreKeyframe(time, translation, rotation);
    }

    CoreAnimationPtr makeClip(float duration, const std::vector<CalCoreTrack>& tracks) {
        CoreAnimationPtr clip = ISTD::make_shared<CalCoreAnimation>();
        clip->duration = duration;
        clip->tracks = tracks;
        return clip;
    }

    ModelSpaceProbe makeProbe(
        const CalVector& point,
        const std::vector<unsigned>& boneIds,
        const std::vector<float>& weights)
    {
        ModelSpaceProbe probe;
        probe.point = point;
        probe.boneIds = boneIds;
        probe.weights = weights;
        return probe;
    }

    // Drive a real CalMixer/CalSkeleton at one time and return the posed
    // skeleton so callers can inspect both bones and skinning transforms.
    CalSkeleton poseAt(const CoreSkeletonPtr& skeleton, const CoreAnimationPtr& clip, float time) {
        CalSkeleton posed(skeleton);
        CalMixer mixer;
        ISTD::shared_ptr<CalAnimation> action =
            ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
        action->time = time;
        mixer.addAnimation(action);
        mixer.updateSkeleton(
            &posed,
            std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());
        return posed;
    }

    // Independent re-implementation of the weighted skinning math, used to
    // cross-check the library's own CalPhysique scalar entry point.
    CalVector directSkin(const CalSkeleton& posed, const ModelSpaceProbe& probe) {
        CalVector accumulated;
        for (size_t k = 0; k < probe.boneIds.size(); ++k) {
            const BoneTransform& m = posed.boneTransforms[probe.boneIds[k]];
            const CalVector& p = probe.point;
            const CalVector transformed(
                m.rowx.x * p.x + m.rowx.y * p.y + m.rowx.z * p.z + m.rowx.w,
                m.rowy.x * p.x + m.rowy.y * p.y + m.rowy.z * p.z + m.rowy.w,
                m.rowz.x * p.x + m.rowz.y * p.y + m.rowz.z * p.z + m.rowz.w);
            accumulated += transformed * probe.weights[k];
        }
        return accumulated;
    }

    ISTD::shared_ptr<CalCoreSubmesh> buildProbeSubmesh(
        const std::vector<ModelSpaceProbe>& probes)
    {
        ISTD::shared_ptr<CalCoreSubmesh> submesh =
            ISTD::make_shared<CalCoreSubmesh>(static_cast<int>(probes.size()), false, 0);
        for (size_t i = 0; i < probes.size(); ++i) {
            CalCoreSubmesh::Vertex vertex;
            vertex.position = CalPoint4(probes[i].point);
            vertex.normal = CalVector4(0, 0, 0);

            std::vector<CalCoreSubmesh::Influence> influences;
            for (size_t k = 0; k < probes[i].boneIds.size(); ++k) {
                const bool last = (k + 1 == probes[i].boneIds.size());
                influences.push_back(
                    CalCoreSubmesh::Influence(probes[i].boneIds[k], probes[i].weights[k], last));
            }
            submesh->addVertex(vertex, 0, influences);
        }
        return submesh;
    }

    // Skin the probes through the actual CalPhysique scalar (x87) routine
    // after posing a real CalSkeleton with a real CalMixer.
    std::vector<CalVector> physiqueSkin(
        const CoreSkeletonPtr& skeleton,
        const CoreAnimationPtr& clip,
        float time,
        const ISTD::shared_ptr<CalCoreSubmesh>& probeSubmesh,
        size_t probeCount)
    {
        CalSkeleton posed = poseAt(skeleton, clip, time);
        CalSubmesh submesh(probeSubmesh);

        std::vector<CalVector4> buffer(2 * probeCount);
        CalPhysique::calculateVerticesAndNormals_x87(
            posed.boneTransforms.data(),
            probeCount,
            cal3d::pointerFromVector(probeSubmesh->getVectorVertex()),
            cal3d::pointerFromVector(probeSubmesh->getInfluences()),
            buffer.data());

        std::vector<CalVector> result(probeCount);
        for (size_t i = 0; i < probeCount; ++i) {
            result[i] = CalVector(buffer[2 * i].x, buffer[2 * i].y, buffer[2 * i].z);
        }
        return result;
    }

    double distance(const CalVector& a, const CalVector& b) {
        const double dx = double(a.x) - double(b.x);
        const double dy = double(a.y) - double(b.y);
        const double dz = double(a.z) - double(b.z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Largest original-vs-candidate probe distance over a sweep of times.
    double sweepMaxError(
        const CoreSkeletonPtr& skeleton,
        const CoreAnimationPtr& original,
        const CoreAnimationPtr& candidate,
        const std::vector<ModelSpaceProbe>& probes,
        const ISTD::shared_ptr<CalCoreSubmesh>& probeSubmesh,
        const std::vector<float>& times)
    {
        double worst = 0.0;
        for (size_t t = 0; t < times.size(); ++t) {
            const std::vector<CalVector> left =
                physiqueSkin(skeleton, original, times[t], probeSubmesh, probes.size());
            const std::vector<CalVector> right =
                physiqueSkin(skeleton, candidate, times[t], probeSubmesh, probes.size());
            for (size_t p = 0; p < probes.size(); ++p) {
                worst = std::max(worst, distance(left[p], right[p]));
            }
        }
        return worst;
    }

    // Rebuild the exact discrete sample set from the spec: 0, duration, all
    // key times, midpoints of consecutive base times, plus the extra times.
    std::vector<float> expectedSampleTimes(
        const CoreAnimationPtr& clip,
        const std::vector<float>& extraSampleTimes)
    {
        std::vector<float> base;
        base.push_back(0.0f);
        base.push_back(clip->duration);
        for (size_t t = 0; t < clip->tracks.size(); ++t) {
            for (size_t k = 0; k < clip->tracks[t].keyframes.size(); ++k) {
                base.push_back(clip->tracks[t].keyframes[k].time);
            }
        }
        for (size_t t = 0; t < extraSampleTimes.size(); ++t) {
            base.push_back(extraSampleTimes[t]);
        }
        std::sort(base.begin(), base.end());
        base.erase(std::unique(base.begin(), base.end()), base.end());

        std::vector<float> all;
        for (size_t i = 0; i < base.size(); ++i) {
            all.push_back(base[i]);
            if (i + 1 < base.size()) {
                all.push_back(0.5f * (base[i] + base[i + 1]));
            }
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        return all;
    }

    size_t totalKeyframes(const CoreAnimationPtr& clip) {
        size_t total = 0;
        for (size_t t = 0; t < clip->tracks.size(); ++t) {
            total += clip->tracks[t].keyframes.size();
        }
        return total;
    }

    std::vector<float> keyTimes(const CalCoreTrack& track) {
        std::vector<float> times;
        for (size_t k = 0; k < track.keyframes.size(); ++k) {
            times.push_back(track.keyframes[k].time);
        }
        return times;
    }

    template<typename Callable>
    bool throwsRuntimeError(Callable callable) {
        try {
            callable();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // 1. A sub-degree swing on a long-chain ancestor must be retained
    //    even though the local-angle compressor is happy to drop it.
    // ------------------------------------------------------------------
    void testLongChain() {
        CoreSkeletonPtr skeleton = ISTD::make_shared<CalCoreSkeleton>();
        skeleton->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        skeleton->addCoreBone(makeBone("arm", 0, CalVector(1, 0, 0)));
        CoreBonePtr tip = makeBone("tip", 1, CalVector(100, 0, 0));
        // Exercise a deliberately non-identity inverse bind pose.
        tip->inverseBindPoseTransform =
            cal3d::RotateTranslate(rotationY(0.3f), CalVector(2, -1, 0.5f));
        skeleton->addCoreBone(tip);

        CalCoreTrack::KeyframeList keys;
        keys.push_back(key(0.0f, CalVector(1, 0, 0), CalQuaternion()));
        keys.push_back(key(0.5f, CalVector(1, 0, 0), rotationZ(0.01f)));
        keys.push_back(key(1.0f, CalVector(1, 0, 0), CalQuaternion()));
        CalCoreTrack track(1, keys);

        CoreAnimationPtr source = makeClip(1.0f, std::vector<CalCoreTrack>(1, track));

        // The legacy local compressor reproduces the devcheck baseline.
        ISTD::shared_ptr<CalCoreTrack> local = track.compress(0.001, 1.0, 0);
        check(local->keyframes.size() == 2, "local compression should drop the mid key");

        std::vector<CalCoreTrack> localTracks(1, *local);
        CoreAnimationPtr locallyCompressed = makeClip(1.0f, localTracks);

        std::vector<ModelSpaceProbe> probes;
        probes.push_back(makeProbe(CalVector(0, 0, 0), std::vector<unsigned>(1, 2), std::vector<float>(1, 1.0f)));
        std::vector<unsigned> multiBones;
        multiBones.push_back(1);
        multiBones.push_back(2);
        std::vector<float> multiWeights;
        multiWeights.push_back(0.5f);
        multiWeights.push_back(0.5f);
        probes.push_back(makeProbe(CalVector(0.5f, 0.5f, 0), multiBones, multiWeights));
        ISTD::shared_ptr<CalCoreSubmesh> submesh = buildProbeSubmesh(probes);

        // The local choice moves the long tip by roughly a whole model unit.
        const double localError =
            sweepMaxError(skeleton, source, locallyCompressed, probes, submesh,
                          std::vector<float>(1, 0.5f));
        check(localError > 0.9, "local compression should produce >0.9 model-space error");

        // Model-space compression with a tighter budget must keep every key.
        const ModelSpaceCompressionResult result =
            source->compressModelSpace(skeleton, probes, std::vector<float>(), 0.5);
        check(result.report.removedKeyframes == 0, "no key may be removed under the tight model budget");
        check(totalKeyframes(result.animation) == 3, "all three original keys must survive");
        check(result.report.maxError < 1e-9, "unchanged animation has zero measured error");

        const std::vector<float> sweepTimes(
            {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f});
        const double residual =
            sweepMaxError(skeleton, source, result.animation, probes, submesh, sweepTimes);
        check(residual < 1e-5, "retained animation must match the original through CalPhysique");

        std::cout << "MODELSPACE_LONG_CHAIN_OK" << std::endl;
    }

    // ------------------------------------------------------------------
    // 2. Deletions that are legal on separate tracks can be illegal once
    //    combined; the global budget must hold for the whole character.
    // ------------------------------------------------------------------
    void testGlobalError() {
        CoreSkeletonPtr skeleton = ISTD::make_shared<CalCoreSkeleton>();
        skeleton->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        skeleton->addCoreBone(makeBone("a1", 0, CalVector(1, 0, 0)));
        skeleton->addCoreBone(makeBone("a2", 1, CalVector(10, 0, 0)));
        skeleton->addCoreBone(makeBone("b1", 0, CalVector(-1, 0, 0)));
        skeleton->addCoreBone(makeBone("b2", 3, CalVector(-10, 0, 0)));

        CalCoreTrack::KeyframeList keysA;
        keysA.push_back(key(0.0f, CalVector(), CalQuaternion()));
        keysA.push_back(key(0.5f, CalVector(), rotationZ(0.01f)));
        keysA.push_back(key(1.0f, CalVector(), CalQuaternion()));

        CalCoreTrack::KeyframeList keysB;
        keysB.push_back(key(0.0f, CalVector(), CalQuaternion()));
        keysB.push_back(key(0.5f, CalVector(), rotationZ(-0.01f)));
        keysB.push_back(key(1.0f, CalVector(), CalQuaternion()));

        std::vector<CalCoreTrack> tracks;
        tracks.push_back(makeTrack(1, keysA));
        tracks.push_back(makeTrack(3, keysB));
        CoreAnimationPtr source = makeClip(1.0f, tracks);

        std::vector<ModelSpaceProbe> probes;
        probes.push_back(makeProbe(
            CalVector(0, 0, 0),
            std::vector<unsigned>({2u, 4u}),
            std::vector<float>({0.5f, 0.5f})));
        probes.push_back(makeProbe(CalVector(0, 0, 0), std::vector<unsigned>(1, 2), std::vector<float>(1, 1.0f)));
        ISTD::shared_ptr<CalCoreSubmesh> submesh = buildProbeSubmesh(probes);

        const double budget = 0.08;
        const ModelSpaceCompressionResult result =
            source->compressModelSpace(skeleton, probes, std::vector<float>(), budget);

        check(result.report.maxError <= budget + 1e-9, "reported error must stay within budget");
        check(result.report.removedKeyframes == 1,
              "exactly one of the two interacting keys may be removed");

        // Removing both middle keys overshoots the shared probe budget.
        std::vector<CalCoreTrack> doubleTracks;
        doubleTracks.push_back(makeTrack(1, CalCoreTrack::KeyframeList(
            {keysA[0], keysA[2]})));
        doubleTracks.push_back(makeTrack(3, CalCoreTrack::KeyframeList(
            {keysB[0], keysB[2]})));
        CoreAnimationPtr bothRemoved = makeClip(1.0f, doubleTracks);

        const std::vector<float> sweepTimes(
            {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f});
        const double combinedError =
            sweepMaxError(skeleton, source, bothRemoved, probes, submesh, sweepTimes);
        check(combinedError > budget, "both deletions together must exceed the budget");

        const double finalError =
            sweepMaxError(skeleton, source, result.animation, probes, submesh, sweepTimes);
        check(finalError <= budget + 1e-6, "independent skinning sweep must confirm the budget");

        std::cout << "MODELSPACE_GLOBAL_ERROR_OK" << std::endl;
    }

    // ------------------------------------------------------------------
    // 3. Truly redundant keys are removed (including antipodal
    //    quaternions and zero tolerance), endpoints are preserved, and
    //    the fixed sample set keeps constraining surviving tracks.
    // ------------------------------------------------------------------
    void testReduction() {
        CoreSkeletonPtr skeleton = ISTD::make_shared<CalCoreSkeleton>();
        skeleton->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        skeleton->addCoreBone(makeBone("longStatic", 0, CalVector(5, 0, 0)));   // bone 1
        skeleton->addCoreBone(makeBone("shortLever", 0, CalVector(0.001f, 0, 0))); // bone 2
        skeleton->addCoreBone(makeBone("antipodal", 0, CalVector(3, 0, 0)));    // bone 3
        skeleton->addCoreBone(makeBone("longMotion", 0, CalVector(50, 0, 0)));  // bone 4

        // Track A: five identical keys -> all three interior keys are exact
        // redundancy even at zero tolerance.
        CalCoreTrack::KeyframeList staticKeys;
        for (size_t k = 0; k < 5; ++k) {
            staticKeys.push_back(key(0.25f * static_cast<float>(k), CalVector(), CalQuaternion()));
        }

        // Track B: a real but tiny swing on a very short lever is negligible
        // in model space, so its middle key can go.
        CalCoreTrack::KeyframeList shortKeys;
        shortKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
        shortKeys.push_back(key(0.5f, CalVector(), rotationZ(0.001f)));
        shortKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));

        // Track C: q and -q encode the same pose.  The interior key is exact
        // redundancy despite the sign flip.
        const CalQuaternion q = rotationZ(0.2f);
        CalCoreTrack::KeyframeList antipodalKeys;
        antipodalKeys.push_back(key(0.0f, CalVector(), q));
        antipodalKeys.push_back(key(0.5f, CalVector(), CalQuaternion(-q.x, -q.y, -q.z, -q.w)));
        antipodalKeys.push_back(key(1.0f, CalVector(), q));

        // Track D: a genuine swing on a long lever.  At the 0.25/0.75 sample
        // times dropping its middle key would move the tip far, so it must be
        // retained even though those times are not key times of this track.
        CalCoreTrack::KeyframeList longKeys;
        longKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
        longKeys.push_back(key(0.5f, CalVector(), rotationZ(0.02f)));
        longKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));

        std::vector<CalCoreTrack> tracks;
        tracks.push_back(makeTrack(1, staticKeys));
        tracks.push_back(makeTrack(2, shortKeys));
        tracks.push_back(makeTrack(3, antipodalKeys));
        tracks.push_back(makeTrack(4, longKeys));
        CoreAnimationPtr source = makeClip(1.0f, tracks);

        std::vector<ModelSpaceProbe> probes;
        // Offset from bone 4's pivot so its rotation actually moves the point.
        probes.push_back(makeProbe(CalVector(51, 0, 0), std::vector<unsigned>(1, 4), std::vector<float>(1, 1.0f)));
        probes.push_back(makeProbe(
            CalVector(1, 1, 1),
            std::vector<unsigned>({1u, 2u, 3u}),
            std::vector<float>({0.4f, 0.3f, 0.3f})));
        ISTD::shared_ptr<CalCoreSubmesh> submesh = buildProbeSubmesh(probes);

        std::vector<float> extraTimes(1, 0.33f);
        const ModelSpaceCompressionResult result =
            source->compressModelSpace(skeleton, probes, extraTimes, 0.001);

        check(result.report.removedKeyframes == 5,
              "three static keys, one short-lever key, one antipodal key removed");
        check(result.report.maxError <= 0.001 + 1e-9, "zero-ish budget respected");

        // A zero error budget still permits removing exactly redundant keys:
        // identical keys and antipodal representations of the same pose.  The
        // non-dyadic extra time forces arbitrary slerp blend factors, where
        // slerp(q, q) is only equal to q up to floating-point rounding.
        {
            std::vector<CalCoreTrack> exactTracks;
            exactTracks.push_back(makeTrack(1, staticKeys));
            exactTracks.push_back(makeTrack(3, antipodalKeys));
            CoreAnimationPtr exactClip = makeClip(1.0f, exactTracks);

            std::vector<float> exactExtra;
            exactExtra.push_back(0.33f);
            exactExtra.push_back(0.66f);
            const ModelSpaceCompressionResult exact =
                exactClip->compressModelSpace(skeleton, probes, exactExtra, 0.0);
            check(exact.report.removedKeyframes == 4,
                  "zero tolerance still removes exactly redundant interior keys");
            check(exact.report.maxError <= 1e-6, "zero tolerance leaves only numerical noise");
        }

        // Endpoints, surviving times and surviving values are untouched.
        check(result.animation->tracks.size() == source->tracks.size(), "track count preserved");
        for (size_t t = 0; t < source->tracks.size(); ++t) {
            const CalCoreTrack& original = source->tracks[t];
            const CalCoreTrack& compressed = *result.animation->getCoreTrack(original.coreBoneId);
            const std::vector<float> originalTimes = keyTimes(original);
            const std::vector<float> compressedTimes = keyTimes(compressed);

            check(compressedTimes.front() == originalTimes.front(), "first key time retained");
            check(compressedTimes.back() == originalTimes.back(), "last key time retained");

            // Every surviving time and value exists verbatim in the source.
            for (size_t k = 0; k < compressed.keyframes.size(); ++k) {
                bool found = false;
                for (size_t j = 0; j < original.keyframes.size(); ++j) {
                    if (compressed.keyframes[k].time == original.keyframes[j].time &&
                        compressed.keyframes[k].transform == original.keyframes[j].transform)
                    {
                        found = true;
                        break;
                    }
                }
                check(found, "surviving key must be an original, unmodified key");
            }
        }

        // The long-motion track keeps all three keys: the fixed sample set
        // still contains 0.25 and 0.75, where deletion would be visible.
        const CalCoreTrack* longTrack = result.animation->getCoreTrack(4);
        check(longTrack && longTrack->keyframes.size() == 3, "long-lever motion key retained");

        std::vector<CalCoreTrack> deletedLongTracks;
        deletedLongTracks.push_back(makeTrack(4, CalCoreTrack::KeyframeList({longKeys[0], longKeys[2]})));
        CoreAnimationPtr deletedLong = makeClip(1.0f, deletedLongTracks);
        const double midError =
            sweepMaxError(skeleton, source, deletedLong, probes, submesh,
                          std::vector<float>(1, 0.25f));
        check(midError > 0.001, "the fixed midpoint sample would catch the deleted key");

        std::cout << "MODELSPACE_REDUCTION_OK" << std::endl;
    }

    // ------------------------------------------------------------------
    // 4. End-to-end verification through CalMixer, CalSkeleton and the
    //    CalPhysique scalar skinning entry, with non-unit inverse binds
    //    and a multi-bone probe.  Also distinguishes empty from missing
    //    tracks and checks the report against an independent sweep.
    // ------------------------------------------------------------------
    void testSkinning() {
        CoreSkeletonPtr skeleton = ISTD::make_shared<CalCoreSkeleton>();
        skeleton->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        skeleton->addCoreBone(makeBone("b1", 0, CalVector(2, 0, 0)));
        skeleton->addCoreBone(makeBone("b2", 0, CalVector(0, 3, 0)));
        skeleton->addCoreBone(makeBone("b3", 0, CalVector(0, 0, 0.25f)));

        CalQuaternion bindRot1;
        bindRot1.setAxisAngle(CalVector(0, 1, 0), 0.3f);
        skeleton->coreBones[1]->inverseBindPoseTransform =
            cal3d::RotateTranslate(bindRot1, CalVector(1.5f, -2.0f, 0.25f));
        CalQuaternion bindRot2;
        bindRot2.setAxisAngle(CalVector(1, 0, 0), -0.4f);
        skeleton->coreBones[2]->inverseBindPoseTransform =
            cal3d::RotateTranslate(bindRot2, CalVector(0.0f, 1.0f, -0.75f));

        CalCoreTrack::KeyframeList keys1;
        keys1.push_back(key(0.0f, CalVector(), CalQuaternion()));
        keys1.push_back(key(0.5f, CalVector(0.1f, 0, 0), rotationZ(0.05f)));
        keys1.push_back(key(1.0f, CalVector(), CalQuaternion()));

        CalCoreTrack::KeyframeList keys2;
        keys2.push_back(key(0.0f, CalVector(), CalQuaternion()));
        keys2.push_back(key(0.5f, CalVector(0, -0.1f, 0), rotationZ(-0.04f)));
        keys2.push_back(key(1.0f, CalVector(), CalQuaternion()));

        CalCoreTrack::KeyframeList staticKeys;
        staticKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
        staticKeys.push_back(key(0.5f, CalVector(), CalQuaternion()));
        staticKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));

        std::vector<CalCoreTrack> tracks;
        tracks.push_back(makeTrack(1, keys1));
        tracks.push_back(makeTrack(2, keys2));
        tracks.push_back(makeTrack(3, staticKeys));
        CoreAnimationPtr source = makeClip(1.0f, tracks);

        std::vector<ModelSpaceProbe> probes;
        probes.push_back(makeProbe(
            CalVector(1, 1, 1),
            std::vector<unsigned>({1u, 2u}),
            std::vector<float>({0.6f, 0.4f})));
        probes.push_back(makeProbe(CalVector(0.5f, 0, 0), std::vector<unsigned>(1, 2), std::vector<float>(1, 1.0f)));
        ISTD::shared_ptr<CalCoreSubmesh> submesh = buildProbeSubmesh(probes);

        const double budget = 0.25;
        const ModelSpaceCompressionResult result =
            source->compressModelSpace(skeleton, probes, std::vector<float>(1, 0.33f), budget);
        check(result.report.removedKeyframes >= 1, "the static track's interior key is redundant");
        check(result.report.maxError <= budget + 1e-9, "reported error within budget");

        // CalPhysique output must agree with independent weighted math, for
        // both the original and the compressed animation.
        const std::vector<float> checkTimes(
            {0.0f, 0.1f, 0.25f, 0.33f, 0.5f, 0.7f, 0.9f, 1.0f});
        for (size_t i = 0; i < checkTimes.size(); ++i) {
            CalSkeleton posedOriginal = poseAt(skeleton, source, checkTimes[i]);
            CalSkeleton posedCompressed = poseAt(skeleton, result.animation, checkTimes[i]);
            const std::vector<CalVector> skinned =
                physiqueSkin(skeleton, source, checkTimes[i], submesh, probes.size());
            const std::vector<CalVector> skinnedCompressed =
                physiqueSkin(skeleton, result.animation, checkTimes[i], submesh, probes.size());
            for (size_t p = 0; p < probes.size(); ++p) {
                const CalVector expected = directSkin(posedOriginal, probes[p]);
                check(distance(expected, skinned[p]) < 1e-5,
                      "CalPhysique must match weighted matrix skinning");
                check(distance(directSkin(posedCompressed, probes[p]), skinnedCompressed[p]) < 1e-5,
                      "CalPhysique must match weighted matrix skinning after compression");
            }
        }

        // Independently reproduce the report over the exact discrete set the
        // compressor is required to honor (not a coarse generic sweep).
        const std::vector<float> specTimes =
            expectedSampleTimes(source, std::vector<float>(1, 0.33f));
        const double independentMax =
            sweepMaxError(skeleton, source, result.animation, probes, submesh, specTimes);
        check(close(independentMax, result.report.maxError,
                    1e-5 * (1.0 + independentMax)),
              "report max error must match the independent sweep");

        // The report's argmax really is the worst probe at the reported time.
        const std::vector<CalVector> atWorstOriginal =
            physiqueSkin(skeleton, source, result.report.maxErrorTime, submesh, probes.size());
        const std::vector<CalVector> atWorstCompressed =
            physiqueSkin(skeleton, result.animation, result.report.maxErrorTime, submesh, probes.size());
        const double atReported = distance(
            atWorstOriginal[result.report.maxErrorProbeIndex],
            atWorstCompressed[result.report.maxErrorProbeIndex]);
        check(close(atReported, result.report.maxError,
                    1e-5 * (1.0 + result.report.maxError)),
              "report time and probe index must identify the worst point");

        // The inputs are untouched.
        check(totalKeyframes(source) == 9, "source animation must not be modified");
        check(source->tracks[0].keyframes[1].transform.translation.x == 0.1f,
              "source keyframe values must not be modified");

        // Empty track: getCurrentTransform() yields the identity local pose.
        // Missing track: the bone stays in its bind pose.  These differ and
        // must not be conflated.
        CoreSkeletonPtr twoBone = ISTD::make_shared<CalCoreSkeleton>();
        twoBone->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        twoBone->addCoreBone(makeBone("child", 0, CalVector(7, 0, 0)));

        CalCoreTrack::KeyframeList rootKeys;
        rootKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
        rootKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));

        std::vector<CalCoreTrack> withEmpty;
        withEmpty.push_back(makeTrack(0, rootKeys));
        withEmpty.push_back(makeTrack(1, CalCoreTrack::KeyframeList()));
        CoreAnimationPtr emptyTrackClip = makeClip(1.0f, withEmpty);

        std::vector<CalCoreTrack> withMissing;
        withMissing.push_back(makeTrack(0, rootKeys));
        CoreAnimationPtr missingTrackClip = makeClip(1.0f, withMissing);

        CalSkeleton emptyPose = poseAt(twoBone, emptyTrackClip, 0.5f);
        CalSkeleton missingPose = poseAt(twoBone, missingTrackClip, 0.5f);
        check(emptyPose.bones[1].absoluteTransform.translation.x == 0.0f,
              "empty track poses the bone with an identity local transform");
        check(missingPose.bones[1].absoluteTransform.translation.x == 7.0f,
              "missing track leaves the bone in bind pose");

        std::vector<ModelSpaceProbe> childProbe;
        childProbe.push_back(makeProbe(CalVector(0, 0, 0), std::vector<unsigned>(1, 1), std::vector<float>(1, 1.0f)));
        const ModelSpaceCompressionResult emptyResult =
            emptyTrackClip->compressModelSpace(twoBone, childProbe, std::vector<float>(), 0.0);
        const ModelSpaceCompressionResult missingResult =
            missingTrackClip->compressModelSpace(twoBone, childProbe, std::vector<float>(), 0.0);
        check(emptyResult.report.maxError < 1e-9 && missingResult.report.maxError < 1e-9,
              "empty and missing tracks both compress against themselves with zero error");

        std::cout << "MODELSPACE_SKINNING_OK" << std::endl;
    }

    // ------------------------------------------------------------------
    // 5. Invalid inputs are rejected with an exception and leave no
    //    partial result behind.
    // ------------------------------------------------------------------
    void testInvalid() {
        CoreSkeletonPtr skeleton = ISTD::make_shared<CalCoreSkeleton>();
        skeleton->addCoreBone(makeBone("root", -1, CalVector(0, 0, 0)));
        skeleton->addCoreBone(makeBone("child", 0, CalVector(1, 0, 0)));

        CalCoreTrack::KeyframeList goodKeys;
        goodKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
        goodKeys.push_back(key(0.5f, CalVector(), rotationZ(0.01f)));
        goodKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));

        std::vector<ModelSpaceProbe> probes;
        probes.push_back(makeProbe(
            CalVector(0, 0, 0),
            std::vector<unsigned>({0u, 1u}),
            std::vector<float>({0.5f, 0.5f})));

        auto validClip = [&]() { return makeClip(1.0f, std::vector<CalCoreTrack>(1, makeTrack(1, goodKeys))); };

        // Duplicate bone id tracks.
        {
            std::vector<CalCoreTrack> tracks;
            tracks.push_back(makeTrack(0, goodKeys));
            tracks.push_back(makeTrack(0, goodKeys));
            CoreAnimationPtr clip = makeClip(1.0f, tracks);
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "duplicate track bone ids must be rejected");
        }

        // Non-strictly-increasing key times.
        {
            CalCoreTrack::KeyframeList badKeys;
            badKeys.push_back(key(0.5f, CalVector(), CalQuaternion()));
            badKeys.push_back(key(0.5f, CalVector(), CalQuaternion()));
            CoreAnimationPtr clip = makeClip(1.0f, std::vector<CalCoreTrack>(1, makeTrack(1, badKeys)));
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "equal key times must be rejected");
        }

        // Out-of-range key times.
        {
            CalCoreTrack::KeyframeList badKeys;
            badKeys.push_back(key(0.0f, CalVector(), CalQuaternion()));
            badKeys.push_back(key(1.5f, CalVector(), CalQuaternion()));
            CoreAnimationPtr clip = makeClip(1.0f, std::vector<CalCoreTrack>(1, makeTrack(1, badKeys)));
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "key time beyond duration must be rejected");
        }
        {
            CalCoreTrack::KeyframeList badKeys;
            badKeys.push_back(key(-0.1f, CalVector(), CalQuaternion()));
            CoreAnimationPtr clip = makeClip(1.0f, std::vector<CalCoreTrack>(1, makeTrack(1, badKeys)));
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "negative key time must be rejected");
        }

        // Illegal weights.
        {
            std::vector<ModelSpaceProbe> bad = probes;
            bad[0].weights[0] = -0.5f;
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, bad, std::vector<float>(), 0.1);
            }), "negative weight must be rejected");
        }
        {
            std::vector<ModelSpaceProbe> bad = probes;
            bad[0].weights[0] = 0.5f;
            bad[0].weights[1] = 0.4f; // sums to 0.9
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, bad, std::vector<float>(), 0.1);
            }), "weights not summing to one must be rejected");
        }
        {
            std::vector<ModelSpaceProbe> bad(1, makeProbe(
                CalVector(0, 0, 0), std::vector<unsigned>(), std::vector<float>()));
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, bad, std::vector<float>(), 0.1);
            }), "probe without influences must be rejected");
        }

        // Unknown bones.
        {
            CoreAnimationPtr clip = makeClip(
                1.0f, std::vector<CalCoreTrack>(1, makeTrack(99, goodKeys)));
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "track with unknown bone must be rejected");
        }
        {
            std::vector<ModelSpaceProbe> bad(1, makeProbe(
                CalVector(0, 0, 0), std::vector<unsigned>(1, 99), std::vector<float>(1, 1.0f)));
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, bad, std::vector<float>(), 0.1);
            }), "probe with unknown bone must be rejected");
        }

        // Non-finite numbers.
        {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            CalCoreTrack::KeyframeList badKeys;
            badKeys.push_back(key(0.0f, CalVector(nan, 0, 0), CalQuaternion()));
            badKeys.push_back(key(1.0f, CalVector(), CalQuaternion()));
            CoreAnimationPtr clip = makeClip(1.0f, std::vector<CalCoreTrack>(1, makeTrack(1, badKeys)));
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.1);
            }), "non-finite keyframe value must be rejected");
        }
        {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            std::vector<ModelSpaceProbe> bad(1, makeProbe(
                CalVector(nan, 0, 0), std::vector<unsigned>(1, 1), std::vector<float>(1, 1.0f)));
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, bad, std::vector<float>(), 0.1);
            }), "non-finite probe point must be rejected");
        }
        {
            const float inf = std::numeric_limits<float>::infinity();
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(1, inf), 0.1);
            }), "non-finite extra sample time must be rejected");
        }
        {
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(1, 1.5f), 0.1);
            }), "out-of-range extra sample time must be rejected");
        }

        // Negative error limit; zero is legal.
        {
            CoreAnimationPtr clip = validClip();
            check(throwsRuntimeError([&]() {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), -0.01);
            }), "negative error limit must be rejected");
        }
        {
            CoreAnimationPtr clip = validClip();
            const ModelSpaceCompressionResult zeroLimit =
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), 0.0);
            check(zeroLimit.report.maxError <= 1e-6, "zero tolerance must run and keep exact poses");
        }

        // A rejected call leaves the inputs intact.
        {
            CoreAnimationPtr clip = validClip();
            const size_t before = totalKeyframes(clip);
            const float savedTime = clip->tracks[0].keyframes[1].time;
            bool threw = false;
            try {
                clip->compressModelSpace(skeleton, probes, std::vector<float>(), -1.0);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            check(threw, "negative limit must throw");
            check(totalKeyframes(clip) == before, "failed call must not alter the animation");
            check(clip->tracks[0].keyframes[1].time == savedTime,
                  "failed call must not alter keyframe times");
        }

        std::cout << "MODELSPACE_INVALID_OK" << std::endl;
    }

} // namespace

int main() {
    testLongChain();
    testGlobalError();
    testReduction();
    testSkinning();
    testInvalid();
    return 0;
}
