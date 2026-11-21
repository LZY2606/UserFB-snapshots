#include "normal_moments.hpp"
#include <cmath>
#include <cassert>

namespace cr
{
    namespace
    {
        unsigned mip_dim(unsigned base, unsigned level)
        {
            unsigned d = base;
            for (unsigned i = 0; i < level; i++) {
                d = (d + 1u) / 2u; // ceiling half
            }
            return d;
        }

        // floor that returns a signed integer (avoids unsigned wrap for negatives)
        int ifloor(float x)
        {
            return int(std::floor(x));
        }

        float fract_signed(float x)
        {
            return x - std::floor(x);
        }
    }

    normal_moments_texture::normal_moments_texture(unsigned width, unsigned height, const float* normals)
    {
        build(width, height, normals);
    }

    normal_moments_texture::normal_moments_texture(unsigned width, unsigned height, const vector<float>& normals)
    {
        assert(normals.size() == width * height * 3u);
        build(width, height, normals.data());
    }

    void normal_moments_texture::build(unsigned width, unsigned height, const float* normals)
    {
        assert(width > 0 && height > 0);
        assert(normals != nullptr);

        base_w = width;
        base_h = height;

        const unsigned total = width * height;
        for (unsigned i = 0; i < total; i++)
        {
            assert(std::isfinite(normals[i * 3 + 0]));
            assert(std::isfinite(normals[i * 3 + 1]));
            assert(std::isfinite(normals[i * 3 + 2]));
            assert(normals[i * 3 + 2] > 0.f);
        }

        // base level: five raw moments of p = nx/nz, q = ny/nz
        vector<moments> base(total);
        for (unsigned i = 0; i < total; i++)
        {
            const float nx = normals[i * 3 + 0];
            const float ny = normals[i * 3 + 1];
            const float nz = normals[i * 3 + 2];
            const float p = nx / nz;
            const float q = ny / nz;
            base[i] = {p, q, p * p, p * q, q * q};
        }
        levels.push_back(std::move(base));

        // mip pyramid, each dimension rounds up; the last level is the whole-image statistic
        while (!(levels.back().size() == 1))
        {
            const unsigned level = unsigned(levels.size());
            const unsigned prev_w = mip_dim(width, level - 1);
            const unsigned prev_h = mip_dim(height, level - 1);
            const unsigned cur_w = mip_dim(width, level);
            const unsigned cur_h = mip_dim(height, level);
            const vector<moments>& prev = levels.back();

            vector<moments> cur(cur_w * cur_h, moments{0.f, 0.f, 0.f, 0.f, 0.f});
            for (unsigned cy = 0; cy < cur_h; cy++)
            {
                for (unsigned cx = 0; cx < cur_w; cx++)
                {
                    // 2x2 source sub-grid; missing children at odd edges simply do not exist,
                    // and each valid child contributes the base texel count it represents
                    moments accum{0.f, 0.f, 0.f, 0.f, 0.f};
                    unsigned weight = 0;
                    for (unsigned oy = 0; oy < 2; oy++)
                    {
                        for (unsigned ox = 0; ox < 2; ox++)
                        {
                            const int px = int(cx * 2 + ox);
                            const int py = int(cy * 2 + oy);
                            if (px >= int(prev_w) || py >= int(prev_h)) {
                                continue;
                            }
                            // number of base texels covered by this child texel
                            const unsigned child_level = level - 1;
                            const unsigned x0 = unsigned(px) * (1u << child_level);
                            const unsigned y0 = unsigned(py) * (1u << child_level);
                            const unsigned x1 = std::min(x0 + (1u << child_level), width);
                            const unsigned y1 = std::min(y0 + (1u << child_level), height);
                            const unsigned w = (x1 - x0) * (y1 - y0);

                            const moments& src = prev[py * prev_w + px];
                            for (unsigned k = 0; k < MOMENT_COUNT; k++) {
                                accum[k] += src[k] * float(w);
                            }
                            weight += w;
                        }
                    }
                    assert(weight > 0);
                    const float inv = 1.f / float(weight);
                    for (unsigned k = 0; k < MOMENT_COUNT; k++) {
                        cur[cy * cur_w + cx][k] = accum[k] * inv;
                    }
                }
            }
            levels.push_back(std::move(cur));
        }
    }

    unsigned normal_moments_texture::level_width(unsigned level) const
    {
        assert(level < levels.size());
        return mip_dim(base_w, level);
    }

    unsigned normal_moments_texture::level_height(unsigned level) const
    {
        assert(level < levels.size());
        return mip_dim(base_h, level);
    }

    const normal_moments_texture::moments&
    normal_moments_texture::texel(unsigned level, unsigned x, unsigned y) const
    {
        assert(level < levels.size());
        assert(x < level_width(level) && y < level_height(level));
        return levels[level][y * level_width(level) + x];
    }

    const normal_moments_texture::moments&
    normal_moments_texture::level_texel(unsigned level, int x, int y) const
    {
        const int w = int(level_width(level));
        const int h = int(level_height(level));
        // repeat addressing, including negative coordinates
        int rx = x % w;
        if (rx < 0) rx += w;
        int ry = y % h;
        if (ry < 0) ry += h;
        return levels[level][ry * w + rx];
    }

    normal_moments_texture::moments
    normal_moments_texture::fetch_bilinear(unsigned level, float u, float v) const
    {
        assert(level < levels.size());
        const float w = float(level_width(level));
        const float h = float(level_height(level));

        // u*w - 0.5 convention; repeat happens only while addressing
        const float fx = u * w - 0.5f;
        const float fy = v * h - 0.5f;
        const int x0 = ifloor(fx);
        const int y0 = ifloor(fy);
        const float tx = fract_signed(fx);
        const float ty = fract_signed(fy);

        const moments& m00 = level_texel(level, x0, y0);
        const moments& m10 = level_texel(level, x0 + 1, y0);
        const moments& m01 = level_texel(level, x0, y0 + 1);
        const moments& m11 = level_texel(level, x0 + 1, y0 + 1);

        moments out{};
        for (unsigned k = 0; k < MOMENT_COUNT; k++)
        {
            const float top = m00[k] * (1.f - tx) + m10[k] * tx;
            const float bot = m01[k] * (1.f - tx) + m11[k] * tx;
            out[k] = top * (1.f - ty) + bot * ty;
        }
        return out;
    }

    normal_moments_texture::sample_result
    normal_moments_texture::sample(float u, float v,
                                   float dudx, float dvdx,
                                   float dudy, float dvdy) const
    {
        // two uv derivative columns scaled by the base dimensions
        const float ax = dudx * float(base_w);
        const float bx = dvdx * float(base_h);
        const float ay = dudy * float(base_w);
        const float by = dvdy * float(base_h);

        // rho = largest singular value of [[ax, ay], [bx, by]]
        const float a = ax * ax + ay * ay;
        const float b = bx * bx + by * by;
        const float c = ax * bx + ay * by;
        const float tr = a + b;
        const float disc = std::sqrt(std::max(0.f, tr * tr - 4.f * (a * b - c * c)));
        const float s_max = std::sqrt(std::max(0.f, (tr + disc) * 0.5f));
        const float rho = s_max;

        const float last = float(levels.size() - 1);
        const float lod_raw = std::log2(std::max(1.f, rho));
        const float lod = std::min(std::max(lod_raw, 0.f), last);

        // trilinear blend of the five raw moments; means and covariance come later
        const unsigned lo = unsigned(std::floor(lod));
        const unsigned hi = std::min(lo + 1u, unsigned(levels.size() - 1));
        const float t = lod - float(lo);

        const moments mlo = fetch_bilinear(lo, u, v);
        sample_result result;
        if (hi == lo)
        {
            result.m = mlo;
        }
        else
        {
            const moments mhi = fetch_bilinear(hi, u, v);
            for (unsigned k = 0; k < MOMENT_COUNT; k++) {
                result.m[k] = mlo[k] * (1.f - t) + mhi[k] * t;
            }
        }
        result.lod = lod;
        return result;
    }
}
