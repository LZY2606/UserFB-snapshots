//****************************************************************************//
// coreanimation.h                                                            //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <list>
#include "cal3d/global.h"
#include "cal3d/coretrack.h"
#include "cal3d/modelspacecompression.h"

class CalCoreSkeleton;
class CalQuaternion;

class CAL3D_API CalCoreAnimation {
public:
    CalCoreAnimation()
        : duration(0.0f)
    {}

    size_t sizeInBytes() const;
    const CalCoreTrack* getCoreTrack(unsigned coreBoneId) const;

    void scale(float factor);
    void fixup(
        const ISTD::shared_ptr<CalCoreSkeleton>& skeleton,
        cal3d::RotateTranslate rt = cal3d::RotateTranslate(),
        bool doFingerFix = false);
    void optimize();

    // Build an independent animation by deleting internal keyframes only
    // while the whole-character skinning error stays within maxPositionError.
    // Error is the maximum Euclidean distance, over the supplied probes and a
    // fixed discrete sample set, between points skinned from the original
    // animation and from the candidate animation.  Neither this animation nor
    // the skeleton or probes are modified.  Throws std::runtime_error on
    // invalid input; in that case no partial result is produced.
    cal3d::ModelSpaceCompressionResult compressModelSpace(
        const ISTD::shared_ptr<CalCoreSkeleton>& skeleton,
        const std::vector<cal3d::ModelSpaceProbe>& probes,
        const std::vector<float>& extraSampleTimes,
        double maxPositionError) const;

    float duration;
    typedef std::vector<CalCoreTrack> TrackList;
    TrackList tracks;
};
CAL3D_PTR(CalCoreAnimation);

inline bool operator==(const CalCoreAnimation& lhs, const CalCoreAnimation& rhs) {
    return lhs.duration == rhs.duration &&
           lhs.tracks == rhs.tracks;
}
