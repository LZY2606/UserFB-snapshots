#include "normal_moments.hpp"
#include <cmath>
#include <cassert>

namespace cr
{
    namespace
    {
        // Euclidean modulo, works for negative indices (repeat wrapping)
        int wrap(int i, unsigned n)
        {
            int r = i % static_cast<int>(n);
            if (r < 0) {
                r += static_cast<int>(n);
            }
            return r;
        }

        unsigned ceil_half(unsigned n)
        {
            return (n + 1u) / 2u;
        }
    }

    normal_moments_texture::normal_moments_texture(const normal_pixel* data, unsigned width, unsigned height) :
        base_w(width),
        base_h(height)
    {
        assert(data != nullptr);
        assert(width > 0 && height > 0);
        build(data);
    }

    normal_moments_texture::normal_moments_texture(const std::vector<normal_pixel>& data, unsigned width, unsigned height) :
        base_w(width),
        base_h(height)
    {
        assert(data.size() >= static_cast<size_t>(width) * height);
        assert(width > 0 && height > 0);
        build(data.data());
    }

    void normal_moments_texture::build(const normal_pixel* data)
    {
        // decide how many levels until a single texel holds the whole image
        levels = 1;
        unsigned w = base_w;
        unsigned h = base_h;
        level_w.clear();
        level_h.clear();
        level_w.push_back(w);
        level_h.push_back(h);
        while (w > 1u || h > 1u)
        {
            w = ceil_half(w);
            h = ceil_half(h);
            level_w.push_back(w);
            level_h.push_back(h);
            ++levels;
        }

        pyramid.resize(levels);
        // number of base-level texels represented by each stored texel
        std::vector<std::vector<unsigned>> weights(levels);

        // base level: one normal per texel, so each raw moment is the value itself
        pyramid[0].resize(static_cast<size_t>(base_w) * base_h);
        weights[0].assign(static_cast<size_t>(base_w) * base_h, 1u);
        for (unsigned y = 0; y < base_h; y++)
        {
            for (unsigned x = 0; x < base_w; x++)
            {
                const normal_pixel& n = data[static_cast<size_t>(y) * base_w + x];
                assert(std::isfinite(n.nx) && std::isfinite(n.ny) && std::isfinite(n.nz));
                assert(n.nz > 0.f);

                const float p = n.nx / n.nz;
                const float q = n.ny / n.nz;
                moments5& m = pyramid[0][static_cast<size_t>(y) * base_w + x];
                m.m[0] = p;
                m.m[1] = q;
                m.m[2] = p * p;
                m.m[3] = p * q;
                m.m[4] = q * q;
            }
        }

        // pooled levels: 2x2 valid sub-cells, weighted by represented base texels
        for (unsigned lvl = 1; lvl < levels; lvl++)
        {
            const unsigned src_w = level_w[lvl - 1];
            const unsigned src_h = level_h[lvl - 1];
            const unsigned dst_w = level_w[lvl];
            const unsigned dst_h = level_h[lvl];
            const auto& src = pyramid[lvl - 1];
            const auto& src_wts = weights[lvl - 1];

            pyramid[lvl].assign(static_cast<size_t>(dst_w) * dst_h, moments5{});
            weights[lvl].assign(static_cast<size_t>(dst_w) * dst_h, 0u);

            for (unsigned dy = 0; dy < dst_h; dy++)
            {
                for (unsigned dx = 0; dx < dst_w; dx++)
                {
                    moments5& dst = pyramid[lvl][static_cast<size_t>(dy) * dst_w + dx];
                    unsigned& dst_weight = weights[lvl][static_cast<size_t>(dy) * dst_w + dx];

                    for (unsigned sy = 0; sy < 2u; sy++)
                    {
                        for (unsigned sx = 0; sx < 2u; sx++)
                        {
                            const int ix = static_cast<int>(2u * dx + sx);
                            const int iy = static_cast<int>(2u * dy + sy);
                            if (ix >= static_cast<int>(src_w) || iy >= static_cast<int>(src_h)) {
                                continue; // odd dimension: missing sub-cell
                            }

                            const size_t sidx = static_cast<size_t>(iy) * src_w + ix;
                            const unsigned wt = src_wts[sidx];
                            const moments5& sm = src[sidx];
                            for (unsigned k = 0; k < 5u; k++) {
                                dst.m[k] += sm.m[k] * static_cast<float>(wt);
                            }
                            dst_weight += wt;
                        }
                    }

                    assert(dst_weight > 0u);
                    const float inv = 1.f / static_cast<float>(dst_weight);
                    for (unsigned k = 0; k < 5u; k++) {
                        dst.m[k] *= inv;
                    }
                }
            }
        }
    }

    unsigned normal_moments_texture::get_width() const
    {
        return base_w;
    }

    unsigned normal_moments_texture::get_height() const
    {
        return base_h;
    }

    unsigned normal_moments_texture::level_count() const
    {
        return levels;
    }

    unsigned normal_moments_texture::level_width(unsigned level) const
    {
        assert(level < levels);
        return level_w[level];
    }

    unsigned normal_moments_texture::level_height(unsigned level) const
    {
        assert(level < levels);
        return level_h[level];
    }

    const moments5& normal_moments_texture::texel(unsigned level, unsigned x, unsigned y) const
    {
        assert(level < levels);
        assert(x < level_w[level] && y < level_h[level]);
        return pyramid[level][static_cast<size_t>(y) * level_w[level] + x];
    }

    moments5 normal_moments_texture::sample_bilinear(float u, float v, unsigned level) const
    {
        assert(level < levels);
        const unsigned w = level_w[level];
        const unsigned h = level_h[level];
        const auto& data = pyramid[level];

        // texel-center convention: coordinate u*w - 0.5 lands on texel centers
        const float fu = u * static_cast<float>(w) - 0.5f;
        const float fv = v * static_cast<float>(h) - 0.5f;

        const int ix0 = static_cast<int>(std::floor(fu));
        const int iy0 = static_cast<int>(std::floor(fv));
        const float tx = fu - static_cast<float>(ix0);
        const float ty = fv - static_cast<float>(iy0);

        // repeat wrapping applies to addresses only; u/v themselves are not moduloed
        const int x0 = wrap(ix0, w);
        const int x1 = wrap(ix0 + 1, w);
        const int y0 = wrap(iy0, h);
        const int y1 = wrap(iy0 + 1, h);

        const moments5& m00 = data[static_cast<size_t>(y0) * w + x0];
        const moments5& m10 = data[static_cast<size_t>(y0) * w + x1];
        const moments5& m01 = data[static_cast<size_t>(y1) * w + x0];
        const moments5& m11 = data[static_cast<size_t>(y1) * w + x1];

        moments5 out{};
        const float w00 = (1.f - tx) * (1.f - ty);
        const float w10 = tx * (1.f - ty);
        const float w01 = (1.f - tx) * ty;
        const float w11 = tx * ty;
        for (unsigned k = 0; k < 5u; k++)
        {
            out.m[k] = w00 * m00.m[k] + w10 * m10.m[k] +
                       w01 * m01.m[k] + w11 * m11.m[k];
        }
        return out;
    }

    lean_sample normal_moments_texture::sample(float u, float v,
                                               float dudx, float dudy,
                                               float dvdx, float dvdy) const
    {
        // scale both derivative columns by the original image dimensions
        const float ex = dudx * static_cast<float>(base_w);
        const float ey = dudy * static_cast<float>(base_w);
        const float fx = dvdx * static_cast<float>(base_h);
        const float fy = dvdy * static_cast<float>(base_h);

        // largest singular value of M = [[ex, ey], [fx, fy]]
        const float a = ex * ex + fx * fx; // M M^T entries
        const float b = ey * ey + fy * fy;
        const float c = ex * ey + fx * fy;
        const float tr = a + b;
        const float det = a * b - c * c;
        const float disc = std::sqrt(std::max(0.f, tr * tr - 4.f * det));
        const float lambda_max = std::max(0.f, 0.5f * (tr + disc));
        const float rho = std::sqrt(lambda_max);

        const float last = static_cast<float>(levels - 1u);
        const float lod = std::min(last, std::max(0.f, std::log2(std::max(1.f, rho))));

        // trilinear blend between the two surrounding mip levels
        const unsigned l0 = static_cast<unsigned>(std::floor(lod));
        const float t = lod - static_cast<float>(l0);
        const unsigned l1 = std::min(levels - 1u, l0 + 1u);

        const moments5 m0 = sample_bilinear(u, v, l0);
        const moments5 m1 = sample_bilinear(u, v, l1);

        lean_sample out{};
        for (unsigned k = 0; k < 5u; k++)
        {
            // blend the five raw moments first; means/covariance come later
            out.moments.m[k] = (1.f - t) * m0.m[k] + t * m1.m[k];
        }
        out.lod = lod;
        return out;
    }
}
