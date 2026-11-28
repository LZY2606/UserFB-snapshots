//****************************************************************************//
// mixer.cpp                                                                  //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cal3d/error.h"
#include "cal3d/mixer.h"
#include "cal3d/corebone.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/animation.h"

void CalMixer::addAnimation(const ISTD::shared_ptr<CalAnimation>& animation) {
    AnimationList::iterator i = activeAnimations.begin();
    while (i != activeAnimations.end() && animation->priority < (*i)->priority) {
        ++i;
    }
    activeAnimations.insert(i, animation);
}

void CalMixer::removeAnimation(const ISTD::shared_ptr<CalAnimation>& animation) {
    activeAnimations.remove(animation);
}

void CalMixer::updateSkeleton(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments
) {
    skeleton->resetPose();

    auto& bones = skeleton->bones;
    const size_t boneCount = bones.size();

    // Per-bone scale channel state.  This is recomputed from scratch on every
    // update, so no scale can leak in from a previous frame or another model.
    std::vector<CalVector> scaleSums(boneCount, CalVector(0.0f, 0.0f, 0.0f));
    std::vector<float> scaleWeights(boneCount, 0.0f);
    std::vector<float> scaleAttenuation(boneCount, 1.0f);

    // The bone adjustments are "replace" so they have to go first, giving them
    // highest priority and full influence.  Subsequent animations affecting the same bones,
    // including subsequent replace animations, will have their incluence attenuated appropriately.
    applyBoneAdjustments(skeleton, boneTransformAdjustments);

    // loop through all animation actions
    for (auto itaa = activeAnimations.begin(); itaa != activeAnimations.end(); ++itaa) {
        const auto& animation = itaa->get();

        const auto& tracks = animation->coreAnimation->tracks;

        for (auto track = tracks.begin(); track != tracks.end(); ++track) {
            if (track->coreBoneId >= boneCount) {
                continue;
            }

            const unsigned boneId = track->coreBoneId;

            // The scale channel follows the same effective-weight scheme as the
            // pose channel: weight * rampValue, attenuated by higher priority
            // animations that already touched this bone.  Animations without a
            // track for this bone do not consume its remaining coefficient.
            const float scaleWeight = animation->weight * animation->rampValue * scaleAttenuation[boneId];
            if (scaleWeight != 0.0f) {
                scaleSums[boneId] += track->getCurrentScale(animation->time) * scaleWeight;
                scaleWeights[boneId] += scaleWeight;
            }
            if (animation->priority != 0) {
                scaleAttenuation[boneId] *= 1.0f - animation->rampValue;
            }

            bones[boneId].blendPose(
                animation->weight * animation->rampValue,
                track->getCurrentTransform(animation->time),
                // higher priority animations replace 0-priority animations
                animation->priority != 0 ? animation->rampValue : 0.0f);
        }
    }

    // Resolve the animated scale of each bone: weighted arithmetic mean of the
    // contributing tracks, identity when nothing contributed.
    std::vector<CalVector> animatedScales(boneCount, CalVector(1.0f, 1.0f, 1.0f));
    for (size_t boneId = 0; boneId < boneCount; ++boneId) {
        if (scaleWeights[boneId] != 0.0f) {
            animatedScales[boneId] = scaleSums[boneId] / scaleWeights[boneId];
        }
        bones[boneId].scale = cal3d::Scale(animatedScales[boneId]);
    }

    // Explicit scale adjustments multiply component-wise onto the animated
    // scale.  The last adjustment for a bone wins.
    for (size_t i = 0; i < boneScaleAdjustments.size(); i++) {
        const BoneScaleAdjustment& ba = boneScaleAdjustments[i];
        if (ba.boneId < boneCount) {
            bones[ba.boneId].scale = cal3d::Scale(animatedScales[ba.boneId] * ba.scale);
        }
    }

    skeleton->calculateAbsolutePose();
}

void CalMixer::applyBoneAdjustments(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments
) {
    CalSkeleton::BoneArray& bones = skeleton->bones;

    for (size_t i = 0; i < boneTransformAdjustments.size(); ++i) {
        const BoneTransformAdjustment& ba = boneTransformAdjustments[i];
        CalBone& bo = bones[ba.boneId];
        bo.blendPose(
            ba.rampValue, /* weight */
            cal3d::RotateTranslate(
                ba.localOri,
                bo.getOriginalTranslation() /* adjustedLocalPos */),
            ba.rampValue /* subsequentAttenuation */);
    }
}
