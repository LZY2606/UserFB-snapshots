//
// ProbeVolume.cpp
//
// 3D trilinear irradiance probe volume. See ProbeVolume.hpp.
//
// Andrew Willmott
//

#include "ProbeVolume.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace SHL
{
    namespace
    {
        const float kRotTol = 1e-4f;

        bool IsFinite(const Vec3f& v)
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        bool IsFinite(const Mat3f& m)
        {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    if (!std::isfinite(m[i][j]))
                        return false;

            return true;
        }

        float Determinant3(const Mat3f& m)
        {
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                 - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                 + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        }

        // Accept only finite, proper (det +1) orthonormal rotations in the
        // column-vector convention. Reflections, scale and shear are rejected.
        bool IsProperRotation(const Mat3f& m)
        {
            if (!IsFinite(m))
                return false;

            // Columns of m are the local basis axes expressed in world space;
            // a rotation has mutually orthonormal columns.
            for (int i = 0; i < 3; i++)
            {
                Vec3f ci(m[0][i], m[1][i], m[2][i]);

                for (int j = 0; j < 3; j++)
                {
                    Vec3f cj(m[0][j], m[1][j], m[2][j]);
                    float entry = dot(ci, cj);  // (M^T M)_ij
                    float target = (i == j) ? 1.0f : 0.0f;

                    if (std::fabs(entry - target) > kRotTol)
                        return false;
                }
            }

            if (std::fabs(Determinant3(m) - 1.0f) > kRotTol)
                return false;

            return true;
        }
    }

    ProbeVolume::ProbeVolume
    (
        std::array<int, 3>                  size,
        Vec3f                               origin,
        Vec3f                               spacing,
        const std::vector<IrradianceProbe>& probes
    ) :
        mSize(size),
        mOrigin(origin),
        mSpacing(spacing)
    {
        for (int i = 0; i < 3; i++)
            if (mSize[i] <= 0)
                throw std::invalid_argument("ProbeVolume: grid sizes must all be positive");

        if (!IsFinite(mOrigin))
            throw std::invalid_argument("ProbeVolume: origin must be finite");

        for (int i = 0; i < 3; i++)
            if (!std::isfinite(mSpacing[i]) || mSpacing[i] <= 0.0f)
                throw std::invalid_argument("ProbeVolume: spacing must be finite and positive");

        // Two 32-bit ints always fit in int64; the third factor is checked
        // against the int32 bound before multiplying so the product itself
        // cannot overflow.
        int64_t count64 = int64_t(mSize[0]) * int64_t(mSize[1]);

        if (count64 > int64_t(INT32_MAX) / int64_t(mSize[2]))
            throw std::invalid_argument("ProbeVolume: grid size product overflows");

        count64 *= int64_t(mSize[2]);

        int count = int(count64);

        if (size_t(count) != probes.size())
            throw std::invalid_argument("ProbeVolume: probe count does not match grid size product");

        mProbes.resize(size_t(count));
        mAnyValid = false;

        for (int i = 0; i < count; i++)
        {
            const IrradianceProbe& in = probes[i];
            StoredProbe& out = mProbes[i];

            if (!in.valid)
            {
                // Missing probe: its orientation and coefficients are not read.
                out.valid = false;
                continue;
            }

            if (!IsProperRotation(in.localToWorld))
                throw std::invalid_argument("ProbeVolume: localToWorld must be a finite orthonormal rotation with determinant +1");

            for (int c = 0; c < 9; c++)
                for (int k = 0; k < 3; k++)
                    if (!std::isfinite(in.radiance[c][k]))
                        throw std::invalid_argument("ProbeVolume: valid probe radiance coefficients must be finite");

            // Rotate the un-convolved local radiance into the shared world frame.
            // Column-vector convention: CRotateSH takes world = R * local directly.
            CRotateSH(in.localToWorld, 3, in.radiance, out.worldRadiance);
            out.valid = true;
            mAnyValid = true;
        }
    }

    bool ProbeVolume::Sample(Vec3f worldPosition, Vec3f worldNormal, Vec3f& rgb) const
    {
        if (!IsFinite(worldPosition))
            return false;

        if (!IsFinite(worldNormal))
            return false;

        float normalLen2 = sqrlen(worldNormal);

        if (normalLen2 <= 0.0f)
            return false;

        if (!mAnyValid)
            return false;

        Vec3f n = worldNormal / std::sqrt(normalLen2);

        // Grid-space position, clamped to the grid.
        int idx[3][2];
        float w[3][2];

        for (int axis = 0; axis < 3; axis++)
        {
            int nAxis = mSize[axis];

            if (nAxis == 1)
            {
                idx[axis][0] = idx[axis][1] = 0;
                w[axis][0] = 1.0f;
                w[axis][1] = 0.0f;
                continue;
            }

            float t = (worldPosition[axis] - mOrigin[axis]) / mSpacing[axis];
            float hi = float(nAxis - 1);

            if (t < 0.0f)
                t = 0.0f;
            else if (t > hi)
                t = hi;

            int i0 = int(std::floor(t));

            if (i0 < 0)
                i0 = 0;
            else if (i0 >= nAxis)
                i0 = nAxis - 1;

            int i1 = i0 + 1;

            if (i1 >= nAxis)
                i1 = nAxis - 1;

            float f = t - float(i0);

            idx[axis][0] = i0;
            idx[axis][1] = i1;
            w[axis][0] = 1.0f - f;
            w[axis][1] = f;
        }

        Vec4f blended[9];

        for (int c = 0; c < 9; c++)
            blended[c] = Vec4f(vl_0);

        float weightSum = 0.0f;
        int nx = mSize[0];
        int ny = mSize[1];

        for (int bz = 0; bz < 2; bz++)
        {
            for (int by = 0; by < 2; by++)
            {
                for (int bx = 0; bx < 2; bx++)
                {
                    float weight = w[0][bx] * w[1][by] * w[2][bz];

                    if (weight <= 0.0f)
                        continue;  // clamped/singleton axes: never double-count the same probe

                    int linear = idx[0][bx] + nx * (idx[1][by] + ny * idx[2][bz]);
                    const StoredProbe& probe = mProbes[linear];

                    if (!probe.valid)
                        continue;  // missing corner only removes its own geometric weight

                    for (int c = 0; c < 9; c++)
                        blended[c] += probe.worldRadiance[c] * weight;

                    weightSum += weight;
                }
            }
        }

        if (weightSum <= 0.0f)
            return false;  // every contributing corner is missing; no nearest-neighbour fallback

        float invWeight = 1.0f / weightSum;

        for (int c = 0; c < 9; c++)
            blended[c] *= invWeight;

        // Signed coefficients survive the blend; only now convolve for a unit-albedo
        // diffuse surface. ApplyDiffuseBRDF already includes the 1/pi factor.
        ApplyDiffuseBRDF(3, blended);

        Vec4f outgoing = SampleSH(n, 3, blended);

        rgb[0] = std::fmax(outgoing[0], 0.0f);
        rgb[1] = std::fmax(outgoing[1], 0.0f);
        rgb[2] = std::fmax(outgoing[2], 0.0f);

        return true;
    }
}
