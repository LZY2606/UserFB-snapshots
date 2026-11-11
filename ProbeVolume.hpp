//
// ProbeVolume.hpp
//
// A small 3D volume of irradiance probes built on top of the existing
// SH/ZH utilities in SHLib. Each probe stores un-convolved radiance SH
// coefficients in its own local frame; ProbeVolume rotates them into a
// common world frame on construction and performs trilinear, gap-aware
// sampling with diffuse convolution at query time.
//
// Andrew Willmott
//

#ifndef PROBE_VOLUME_H
#define PROBE_VOLUME_H

#include "SHLib.hpp"

#include <array>
#include <vector>

namespace SHL
{
    struct IrradianceProbe
    {
        bool   valid = false;          // false => missing probe, orientation/coeffs are not read
        Mat3f  localToWorld;           // column-vector convention: world = localToWorld * local
        Vec4f  radiance[9];            // un-converted radiance SH, band order matches SampleSH. xyz = linear RGB, w ignored.
    };

    class ProbeVolume
    {
    public:
        ProbeVolume
        (
            std::array<int, 3>                 size,      // grid dimensions, all positive
            Vec3f                              origin,    // world position of grid corner 0
            Vec3f                              spacing,   // world spacing between grid points, all positive
            const std::vector<IrradianceProbe>& probes    // x fastest, then y, then z; copied
        );

        bool Sample(Vec3f worldPosition, Vec3f worldNormal, Vec3f& rgb) const;
        // Sample the volume at worldPosition for the given (not necessarily unit) worldNormal.
        // Positions outside the grid are clamped to it. Missing corner probes only remove their
        // own geometric weight; the remaining positive weights are renormalised. Returns false
        // (leaving rgb untouched) for non-finite inputs, a zero normal, or when no valid probe
        // contributes. On success rgb holds outgoing radiance: unit albedo, diffuse-convolved,
        // divided by pi, with negative components clamped to zero.

    private:
        struct StoredProbe
        {
            bool   valid = false;
            Vec4f  worldRadiance[9];  // rotated into the world frame, still un-convolved
        };

        std::array<int, 3>          mSize;
        Vec3f                       mOrigin;
        Vec3f                       mSpacing;
        std::vector<StoredProbe>    mProbes;
        bool                        mAnyValid = false;
    };
}

#endif
