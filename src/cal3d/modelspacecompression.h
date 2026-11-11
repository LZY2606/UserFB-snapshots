//****************************************************************************//
// modelspacecompression.h                                                    //
// Whole-character (model-space) animation compression entry point.           //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <vector>
#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"
#include "cal3d/vector.h"

class CalCoreAnimation;
class CalCoreSkeleton;

namespace cal3d {

    // A single linear-blend skinning probe used to measure model-space error.
    //
    // 'point' is a position expressed in bind pose.  At evaluation time it is
    // moved through each influence bone's model transform multiplied by that
    // bone's inverse bind pose transform, exactly like regular vertex
    // skinning.  Every weight must be non-negative and the weights of one
    // probe must sum to 1.
    struct ModelSpaceProbe {
        CalVector point;
        std::vector<unsigned> boneIds;
        std::vector<float> weights;
    };

    // Outcome of a model-space compression run.  All numbers are measured
    // against the original animation over the fixed discrete sample set.
    struct ModelSpaceCompressionReport {
        ModelSpaceCompressionReport()
            : removedKeyframes(0)
            , maxError(0.0)
            , maxErrorTime(0.0f)
            , maxErrorProbeIndex(0)
        {}

        // Number of internal keyframes removed across every track.  The first
        // and last keyframe of every non-empty track are always retained.
        unsigned removedKeyframes;

        // Largest Euclidean distance, over every probe and every sample time,
        // between the original and the compressed animation, in model-space
        // position units.
        double maxError;

        // Sample time at which maxError was observed.
        float maxErrorTime;

        // Index (into the probe array passed to compressModelSpace) of the
        // probe at which maxError was observed.
        unsigned maxErrorProbeIndex;
    };

    struct ModelSpaceCompressionResult {
        // Independent animation holding only the surviving keyframes.
        ISTD::shared_ptr<CalCoreAnimation> animation;
        ModelSpaceCompressionReport report;
    };
}
