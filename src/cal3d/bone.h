//****************************************************************************//
// bone.h                                                                     //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include "cal3d/global.h"
#include "cal3d/transform.h"

class CalCoreBone;
class CalSkeleton;
class CalModel;

namespace cal3d {
    class CAL3D_API TransformAccumulator {
    public:
        TransformAccumulator();

        void reset(const RotateTranslate& defaultPose);

        void addTransform(float weight, const RotateTranslate& transform);
        const RotateTranslate& getWeightedMean() const {
            return currentTransform;
        }

    private:
        float totalWeight;
        RotateTranslate currentTransform;
    };
}

class CAL3D_API CalBone {
public:
    const int parentId;
    cal3d::Transform absoluteTransform;

    // Animation-driven local scale: the weighted mean of the scales sampled
    // from the animations affecting this bone.  Identity if none contributed.
    cal3d::Scale scale;

    CalBone(const CalCoreBone& coreBone);

    const cal3d::RotateTranslate& getRelativeTransform() const {
        return transformAccumulator.getWeightedMean();
    }

    const CalVector& getOriginalTranslation() const {
        return coreRelativeTransform.translation;
    }

    void resetPose();
    void blendPose(
        float weight,
        const cal3d::RotateTranslate& transform,
        float subsequentAttenuation);

    // Accumulate an animation's local scale into this bone's scale.  The
    // attenuation applies only to subsequent animations' scale weights, and is
    // independent of the pose attenuation used by blendPose above.
    void blendScalePose(
        float weight,
        const CalVector& scaleValue,
        float subsequentAttenuation);

    // BoneScaleAdjustment is a per-frame multiplier applied on top of the
    // animation scale, not part of the animation weight calculation.
    void setScaleAdjustment(const CalVector& scaleValue) {
        scaleAdjustment.scale = scaleValue;
    }

    void calculateAbsolutePose(const CalBone* bones);

private:
    // from core bone. stored locally for better cache locality
    const cal3d::RotateTranslate coreRelativeTransform;

    // animated bone state
    cal3d::Scale absoluteScale;
    cal3d::Scale scaleAdjustment;
    float currentAttenuation;
    float currentScaleAttenuation;
    float currentScaleWeight;
    cal3d::TransformAccumulator transformAccumulator;
};
