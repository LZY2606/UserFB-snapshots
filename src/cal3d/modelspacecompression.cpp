//****************************************************************************//
// modelspacecompression.cpp                                                  //
// Whole-character (model-space) animation compression.                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cal3d/modelspacecompression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include "cal3d/animation.h"
#include "cal3d/bonetransform.h"
#include "cal3d/coreanimation.h"
#include "cal3d/corebone.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/coretrack.h"
#include "cal3d/mixer.h"
#include "cal3d/skeleton.h"
#include "cal3d/transform.h"

namespace {

    typedef ISTD::shared_ptr<CalCoreSkeleton> CoreSkeletonPtr;
    typedef ISTD::shared_ptr<CalCoreAnimation> CoreAnimationPtr;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool finite(float v) {
        return std::isfinite(v);
    }

    bool finite(double v) {
        return std::isfinite(v);
    }

    bool finite(const CalVector& v) {
        return finite(v.x) && finite(v.y) && finite(v.z);
    }

    bool finite(const CalQuaternion& q) {
        return finite(q.x) && finite(q.y) && finite(q.z) && finite(q.w);
    }

    bool finite(const cal3d::RotateTranslate& t) {
        return finite(t.translation) && finite(t.rotation);
    }

    // ------------------------------------------------------------------
    // Input validation.  Runs before any result object is constructed so
    // that a failure never leaves a partial result behind.
    // ------------------------------------------------------------------
    void validate(
        const CalCoreAnimation& animation,
        const CoreSkeletonPtr& skeleton,
        const std::vector<cal3d::ModelSpaceProbe>& probes,
        const std::vector<float>& extraSampleTimes,
        double maxPositionError)
    {
        require(skeleton.get() != 0, "compressModelSpace: skeleton must not be null");
        require(finite(maxPositionError),
                "compressModelSpace: max position error must be finite");
        require(maxPositionError >= 0.0,
                "compressModelSpace: max position error must not be negative");
        require(finite(animation.duration) && animation.duration >= 0.0f,
                "compressModelSpace: animation duration must be finite and non-negative");

        const size_t boneCount = skeleton->coreBones.size();
        for (size_t boneId = 0; boneId < boneCount; ++boneId) {
            require(finite(skeleton->coreBones[boneId]->relativeTransform),
                    "compressModelSpace: skeleton contains a non-finite bind transform");
            require(finite(skeleton->coreBones[boneId]->inverseBindPoseTransform),
                    "compressModelSpace: skeleton contains a non-finite inverse bind transform");
        }

        // Tracks: unique, known bones; key times strictly increasing and in range.
        std::set<unsigned> usedBoneIds;
        for (size_t trackIndex = 0; trackIndex < animation.tracks.size(); ++trackIndex) {
            const CalCoreTrack& track = animation.tracks[trackIndex];
            require(track.coreBoneId < boneCount,
                    "compressModelSpace: track references an unknown bone");
            require(usedBoneIds.insert(track.coreBoneId).second,
                    "compressModelSpace: duplicate tracks for the same bone id");

            const CalCoreTrack::KeyframeList& keyframes = track.keyframes;
            for (size_t k = 0; k < keyframes.size(); ++k) {
                const CalCoreKeyframe& keyframe = keyframes[k];
                require(finite(keyframe.time),
                        "compressModelSpace: non-finite keyframe time");
                require(keyframe.time >= 0.0f && keyframe.time <= animation.duration,
                        "compressModelSpace: keyframe time outside [0, duration]");
                require(finite(keyframe.transform),
                        "compressModelSpace: non-finite keyframe transform");
                if (k > 0) {
                    require(keyframes[k - 1].time < keyframe.time,
                            "compressModelSpace: keyframe times must be strictly increasing");
                }
            }
        }

        // Probes: known bones, non-negative weights summing to 1.
        for (size_t probeIndex = 0; probeIndex < probes.size(); ++probeIndex) {
            const cal3d::ModelSpaceProbe& probe = probes[probeIndex];
            require(finite(probe.point),
                    "compressModelSpace: non-finite probe point");
            require(probe.boneIds.size() == probe.weights.size() && !probe.boneIds.empty(),
                    "compressModelSpace: probe needs matching, non-empty bone/weight lists");

            float weightSum = 0.0f;
            for (size_t k = 0; k < probe.weights.size(); ++k) {
                require(finite(probe.weights[k]),
                        "compressModelSpace: non-finite probe weight");
                require(probe.weights[k] >= 0.0f,
                        "compressModelSpace: probe weights must be non-negative");
                require(probe.boneIds[k] < boneCount,
                        "compressModelSpace: probe references an unknown bone");
                weightSum += probe.weights[k];
            }
            require(finite(weightSum) && std::fabs(weightSum - 1.0f) <= 1e-5f,
                    "compressModelSpace: probe weights must sum to 1");
        }

        for (size_t i = 0; i < extraSampleTimes.size(); ++i) {
            require(finite(extraSampleTimes[i]),
                    "compressModelSpace: non-finite extra sample time");
            require(extraSampleTimes[i] >= 0.0f && extraSampleTimes[i] <= animation.duration,
                    "compressModelSpace: extra sample time outside [0, duration]");
        }
    }

    // ------------------------------------------------------------------
    // Fixed discrete sample set.  It is derived once and never shrinks,
    // no matter which keyframes are later removed.
    // ------------------------------------------------------------------
    std::vector<float> buildSampleTimes(
        const CalCoreAnimation& animation,
        const std::vector<float>& extraSampleTimes)
    {
        std::vector<float> baseTimes;
        baseTimes.reserve(2 + extraSampleTimes.size());
        baseTimes.push_back(0.0f);
        baseTimes.push_back(animation.duration);
        for (size_t t = 0; t < animation.tracks.size(); ++t) {
            const CalCoreTrack::KeyframeList& keyframes = animation.tracks[t].keyframes;
            for (size_t k = 0; k < keyframes.size(); ++k) {
                baseTimes.push_back(keyframes[k].time);
            }
        }
        for (size_t t = 0; t < extraSampleTimes.size(); ++t) {
            baseTimes.push_back(extraSampleTimes[t]);
        }

        std::sort(baseTimes.begin(), baseTimes.end());
        baseTimes.erase(
            std::unique(baseTimes.begin(), baseTimes.end()),
            baseTimes.end());

        std::vector<float> sampleTimes;
        sampleTimes.reserve(baseTimes.size() * 2);
        for (size_t i = 0; i < baseTimes.size(); ++i) {
            sampleTimes.push_back(baseTimes[i]);
            if (i + 1 < baseTimes.size()) {
                sampleTimes.push_back(0.5f * (baseTimes[i] + baseTimes[i + 1]));
            }
        }

        std::sort(sampleTimes.begin(), sampleTimes.end());
        sampleTimes.erase(
            std::unique(sampleTimes.begin(), sampleTimes.end()),
            sampleTimes.end());
        return sampleTimes;
    }

    // Linear-blend skinning of one probe point through the evaluated pose.
    // Mirrors CalPhysique::calculateVerticesAndNormals_x87: each bone
    // transform is applied to the bind-pose point and results are weighted.
    CalVector skinProbe(
        const cal3d::SSEArray<BoneTransform>& boneTransforms,
        const cal3d::ModelSpaceProbe& probe)
    {
        CalVector accumulated;
        for (size_t k = 0; k < probe.boneIds.size(); ++k) {
            const BoneTransform& matrix = boneTransforms[probe.boneIds[k]];
            const CalVector& point = probe.point;
            const CalVector transformed(
                matrix.rowx.x * point.x + matrix.rowx.y * point.y + matrix.rowx.z * point.z + matrix.rowx.w,
                matrix.rowy.x * point.x + matrix.rowy.y * point.y + matrix.rowy.z * point.z + matrix.rowy.w,
                matrix.rowz.x * point.x + matrix.rowz.y * point.y + matrix.rowz.z * point.z + matrix.rowz.w);
            accumulated += transformed * probe.weights[k];
        }
        return accumulated;
    }

    // Evaluate an animation at one time with a real CalMixer/CalSkeleton and
    // skin every probe.  Missing tracks leave their bones in bind pose;
    // empty tracks contribute the identity local transform through their
    // getCurrentTransform().  Both behaviors come from the runtime itself.
    std::vector<CalVector> evaluateAt(
        const CoreSkeletonPtr& skeleton,
        const CoreAnimationPtr& animation,
        float time,
        const std::vector<cal3d::ModelSpaceProbe>& probes)
    {
        CalSkeleton posedSkeleton(skeleton);
        CalMixer mixer;
        ISTD::shared_ptr<CalAnimation> action =
            ISTD::make_shared<CalAnimation>(animation, 1.0f, 0);
        action->time = time;
        mixer.addAnimation(action);
        mixer.updateSkeleton(
            &posedSkeleton,
            std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());

        std::vector<CalVector> skinned;
        skinned.reserve(probes.size());
        for (size_t p = 0; p < probes.size(); ++p) {
            skinned.push_back(skinProbe(posedSkeleton.boneTransforms, probes[p]));
        }
        return skinned;
    }

    typedef std::vector<std::vector<CalVector> > SkinnedPoseSet;

    SkinnedPoseSet evaluateAll(
        const CoreSkeletonPtr& skeleton,
        const CoreAnimationPtr& animation,
        const std::vector<float>& sampleTimes,
        const std::vector<cal3d::ModelSpaceProbe>& probes)
    {
        SkinnedPoseSet result;
        result.reserve(sampleTimes.size());
        for (size_t s = 0; s < sampleTimes.size(); ++s) {
            result.push_back(evaluateAt(skeleton, animation, sampleTimes[s], probes));
        }
        return result;
    }

    struct ErrorMeasure {
        ErrorMeasure()
            : value(0.0)
            , time(0.0f)
            , probeIndex(0)
        {}

        double value;
        float time;
        unsigned probeIndex;
    };

    double distanceSquared(const CalVector& a, const CalVector& b) {
        const double dx = double(a.x) - double(b.x);
        const double dy = double(a.y) - double(b.y);
        const double dz = double(a.z) - double(b.z);
        return dx * dx + dy * dy + dz * dz;
    }

    // Maximum Euclidean distance over every sample time and probe.
    ErrorMeasure measureError(
        const SkinnedPoseSet& reference,
        const SkinnedPoseSet& candidate,
        const std::vector<float>& sampleTimes)
    {
        ErrorMeasure worst;
        for (size_t s = 0; s < reference.size(); ++s) {
            for (size_t p = 0; p < reference[s].size(); ++p) {
                const double d2 = distanceSquared(reference[s][p], candidate[s][p]);
                if (d2 > worst.value * worst.value) {
                    worst.value = std::sqrt(d2);
                    worst.time = sampleTimes[s];
                    worst.probeIndex = static_cast<unsigned>(p);
                }
            }
        }
        return worst;
    }

    CoreAnimationPtr makeAnimation(
        float duration,
        const CalCoreAnimation::TrackList& tracks)
    {
        CoreAnimationPtr animation = ISTD::make_shared<CalCoreAnimation>();
        animation->duration = duration;
        animation->tracks = tracks;
        return animation;
    }

    CalCoreAnimation::TrackList withKeyframeRemoved(
        const CalCoreAnimation::TrackList& tracks,
        size_t trackIndex,
        size_t keyframeIndex)
    {
        CalCoreAnimation::TrackList result = tracks;
        CalCoreTrack& track = result[trackIndex];
        CalCoreTrack::KeyframeList keyframes;
        keyframes.reserve(track.keyframes.size() - 1);
        for (size_t k = 0; k < track.keyframes.size(); ++k) {
            if (k != keyframeIndex) {
                keyframes.push_back(track.keyframes[k]);
            }
        }
        const bool translationRequired = track.translationRequired;
        const bool translationIsDynamic = track.translationIsDynamic;
        track = CalCoreTrack(track.coreBoneId, keyframes);
        // Existing translation flags are conservatively preserved: removing
        // keys must never silently drop the translation channel.
        track.translationRequired = translationRequired;
        track.translationIsDynamic = translationIsDynamic;
        return result;
    }

    struct InternalKeyframe {
        size_t trackIndex;
        size_t keyframeIndex;
        unsigned boneId;
        float time;
    };

    std::vector<InternalKeyframe> listInternalKeyframes(
        const CalCoreAnimation::TrackList& tracks)
    {
        std::vector<InternalKeyframe> keys;
        for (size_t t = 0; t < tracks.size(); ++t) {
            // Only strictly interior keyframes can be deleted; the first and
            // last key of every non-empty track are retained.
            if (tracks[t].keyframes.size() >= 3) {
                for (size_t k = 1; k + 1 < tracks[t].keyframes.size(); ++k) {
                    InternalKeyframe key;
                    key.trackIndex = t;
                    key.keyframeIndex = k;
                    key.boneId = tracks[t].coreBoneId;
                    key.time = tracks[t].keyframes[k].time;
                    keys.push_back(key);
                }
            }
        }
        return keys;
    }

} // namespace

cal3d::ModelSpaceCompressionResult CalCoreAnimation::compressModelSpace(
    const ISTD::shared_ptr<CalCoreSkeleton>& skeleton,
    const std::vector<cal3d::ModelSpaceProbe>& probes,
    const std::vector<float>& extraSampleTimes,
    double maxPositionError) const
{
    validate(*this, skeleton, probes, extraSampleTimes, maxPositionError);

    const std::vector<float> sampleTimes = buildSampleTimes(*this, extraSampleTimes);

    // The original animation's pose over the fixed sample set is the
    // immutable reference for the whole run.
    CoreAnimationPtr source = makeAnimation(duration, tracks);
    const SkinnedPoseSet reference =
        evaluateAll(skeleton, source, sampleTimes, probes);

    // Scale of the model, used to size a floating-point noise allowance.  A
    // zero budget must still admit exactly redundant keys, but slerp(q, q) at
    // an arbitrary blend factor returns q scaled by ((1-f)+f), which need not
    // be bit-identical to q; the allowance covers that rounding and nothing
    // more.  It is many orders of magnitude below any meaningful tolerance.
    double modelScale = 1.0;
    for (size_t s = 0; s < reference.size(); ++s) {
        for (size_t p = 0; p < reference[s].size(); ++p) {
            const CalVector& point = reference[s][p];
            modelScale = std::max(modelScale, double(std::fabs(point.x)));
            modelScale = std::max(modelScale, double(std::fabs(point.y)));
            modelScale = std::max(modelScale, double(std::fabs(point.z)));
        }
    }
    const double numericFloor = 1e-7 * (1.0 + modelScale);
    const double feasibleLimit = maxPositionError + numericFloor;

    CalCoreAnimation::TrackList candidateTracks = tracks;
    unsigned removedKeyframes = 0;

    // Greedy: each round deletes the single interior key whose removal gives
    // the smallest global error, provided the global error stays in limit.
    // Every candidate is re-evaluated against the full current animation, so
    // deletions that are legal separately but illegal together are caught.
    while (true) {
        const std::vector<InternalKeyframe> candidates =
            listInternalKeyframes(candidateTracks);
        if (candidates.empty()) {
            break;
        }

        bool foundFeasible = false;
        InternalKeyframe bestKey;
        double bestError = std::numeric_limits<double>::infinity();

        for (size_t c = 0; c < candidates.size(); ++c) {
            const InternalKeyframe& key = candidates[c];
            CalCoreAnimation::TrackList trialTracks =
                withKeyframeRemoved(candidateTracks, key.trackIndex, key.keyframeIndex);
            CoreAnimationPtr trial = makeAnimation(duration, trialTracks);
            const SkinnedPoseSet trialPose =
                evaluateAll(skeleton, trial, sampleTimes, probes);
            const ErrorMeasure trialError =
                measureError(reference, trialPose, sampleTimes);

            if (trialError.value > feasibleLimit) {
                continue;
            }

            // Tie-break: smallest error, then bone id, then original key time.
            const bool better =
                trialError.value < bestError ||
                (trialError.value == bestError &&
                    (key.boneId < bestKey.boneId ||
                        (key.boneId == bestKey.boneId && key.time < bestKey.time)));
            if (!foundFeasible || better) {
                foundFeasible = true;
                bestError = trialError.value;
                bestKey = key;
            }
        }

        if (!foundFeasible) {
            break;
        }

        candidateTracks =
            withKeyframeRemoved(candidateTracks, bestKey.trackIndex, bestKey.keyframeIndex);
        ++removedKeyframes;
    }

    CoreAnimationPtr compressed = makeAnimation(duration, candidateTracks);

    // The report is a fresh measurement of the final animation against the
    // original, not an accumulation of per-round errors.
    const SkinnedPoseSet finalPose =
        evaluateAll(skeleton, compressed, sampleTimes, probes);
    const ErrorMeasure finalError =
        measureError(reference, finalPose, sampleTimes);

    cal3d::ModelSpaceCompressionResult result;
    result.animation = compressed;
    result.report.removedKeyframes = removedKeyframes;
    result.report.maxError = finalError.value;
    result.report.maxErrorTime = finalError.time;
    result.report.maxErrorProbeIndex = finalError.probeIndex;
    return result;
}
