// https://github.com/dominiksalvet/cpurast

#include "normal_moments.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

using std::max;
using std::min;
using std::floor;
using std::fmod;

namespace cr
{
    namespace
    {
        unsigned ceil_half(unsigned v) {
            return (v + 1) / 2;
        }

        // repeat addressing of an integer texel index (may be negative)
        unsigned repeat_index(double index, unsigned size)
        {
            double m = fmod(index, static_cast<double>(size));
            if (m < 0) {
                m += size;
            }
            return static_cast<unsigned>(m);
        }

        normal_moment_moments operator+(const normal_moment_moments& a, const normal_moment_moments& b)
        {
            normal_moment_moments r;
            r.p = a.p + b.p;
            r.q = a.q + b.q;
            r.pp = a.pp + b.pp;
            r.pq = a.pq + b.pq;
            r.qq = a.qq + b.qq;
            return r;
        }

        normal_moment_moments operator*(const normal_moment_moments& a, double s)
        {
            normal_moment_moments r;
            r.p = a.p * s;
            r.q = a.q * s;
            r.pp = a.pp * s;
            r.pq = a.pq * s;
            r.qq = a.qq * s;
            return r;
        }
    }

    normal_moments::normal_moments(const float* normals, unsigned width, unsigned height) :
        base_width(width),
        base_height(height)
    {
        assert(width > 0 && height > 0);
        build(normals);
    }

    normal_moments::normal_moments(const std::vector<float>& normals, unsigned width, unsigned height) :
        base_width(width),
        base_height(height)
    {
        assert(width > 0 && height > 0);
        assert(normals.size() >= static_cast<size_t>(width) * height * 3);
        build(normals.data());
    }

    void normal_moments::build(const float* normals)
    {
        assert(normals != nullptr);

        level base;
        base.width = base_width;
        base.height = base_height;
        base.cells.resize(static_cast<size_t>(base_width) * base_height);

        const size_t count = static_cast<size_t>(base_width) * base_height;
        for (size_t i = 0; i < count; i++)
        {
            const double nx = normals[i * 3 + 0];
            const double ny = normals[i * 3 + 1];
            const double nz = normals[i * 3 + 2];

            assert(std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz));
            assert(nz > 0);

            const double p = nx / nz;
            const double q = ny / nz;

            // base texels carry weight one
            base.cells[i].weight = 1.0;
            base.cells[i].m.p = p;
            base.cells[i].m.q = q;
            base.cells[i].m.pp = p * p;
            base.cells[i].m.pq = p * q;
            base.cells[i].m.qq = q * q;
        }

        levels.clear();
        levels.push_back(std::move(base));

        while (levels.back().width > 1 || levels.back().height > 1)
        {
            level next;
            build_next_level(levels.back(), next);
            levels.push_back(std::move(next));
        }
    }

    void normal_moments::build_next_level(const level& prev, level& next) const
    {
        next.width = ceil_half(prev.width);
        next.height = ceil_half(prev.height);
        next.cells.assign(static_cast<size_t>(next.width) * next.height, cell{});

        for (unsigned y = 0; y < next.height; y++)
        {
            for (unsigned x = 0; x < next.width; x++)
            {
                cell& dst = next.cells[static_cast<size_t>(y) * next.width + x];

                double weight = 0;
                normal_moment_moments moments;

                // valid 2x2 sub-cells, weighted by represented base texels
                for (unsigned sy = 0; sy < 2; sy++)
                {
                    for (unsigned sx = 0; sx < 2; sx++)
                    {
                        const unsigned px = 2 * x + sx;
                        const unsigned py = 2 * y + sy;
                        if (px >= prev.width || py >= prev.height) {
                            continue;
                        }

                        const cell& src = prev.cells[static_cast<size_t>(py) * prev.width + px];
                        weight += src.weight;
                        moments = moments + src.m * src.weight;
                    }
                }

                assert(weight > 0);
                dst.weight = weight;
                if (weight > 0) {
                    dst.m = moments * (1.0 / weight);
                }
            }
        }
    }

    std::vector<unsigned> normal_moments::level_size(unsigned level_index) const
    {
        assert(level_index < levels.size());
        return {levels[level_index].width, levels[level_index].height};
    }

    normal_moment_moments normal_moments::moments(unsigned level_index, unsigned x, unsigned y) const
    {
        assert(level_index < levels.size());
        const level& l = levels[level_index];
        assert(x < l.width && y < l.height);
        return l.cells[static_cast<size_t>(y) * l.width + x].m;
    }

    double normal_moments::cell_weight(unsigned level_index, unsigned x, unsigned y) const
    {
        assert(level_index < levels.size());
        const level& l = levels[level_index];
        assert(x < l.width && y < l.height);
        return l.cells[static_cast<size_t>(y) * l.width + x].weight;
    }

    normal_moment_moments normal_moments::sample_level(unsigned level_index, double u, double v) const
    {
        assert(level_index < levels.size());
        const level& l = levels[level_index];

        // u * w - 0.5 gives the texel-center position for bilinear filtering
        const double px = u * static_cast<double>(l.width) - 0.5;
        const double py = v * static_cast<double>(l.height) - 0.5;

        const double fx0 = floor(px);
        const double fy0 = floor(py);
        const double fx = px - fx0;
        const double fy = py - fy0;

        // repeat addressing only; the original texture coordinates are never
        // reduced modulo one beforehand
        const unsigned x0 = repeat_index(fx0, l.width);
        const unsigned x1 = repeat_index(fx0 + 1.0, l.width);
        const unsigned y0 = repeat_index(fy0, l.height);
        const unsigned y1 = repeat_index(fy0 + 1.0, l.height);

        const cell& c00 = l.cells[static_cast<size_t>(y0) * l.width + x0];
        const cell& c10 = l.cells[static_cast<size_t>(y0) * l.width + x1];
        const cell& c01 = l.cells[static_cast<size_t>(y1) * l.width + x0];
        const cell& c11 = l.cells[static_cast<size_t>(y1) * l.width + x1];

        const double w00 = (1.0 - fx) * (1.0 - fy);
        const double w10 = fx * (1.0 - fy);
        const double w01 = (1.0 - fx) * fy;
        const double w11 = fx * fy;

        normal_moment_moments r = c00.m * w00;
        r = r + c10.m * w10;
        r = r + c01.m * w01;
        r = r + c11.m * w11;
        return r;
    }

    normal_moment_footprint normal_moments::footprint(double dx_u, double dx_v,
                                                      double dy_u, double dy_v) const
    {
        // Jacobian columns (texture coordinate per canvas pixel), each scaled by
        // its own original image dimension:
        //   d(texel)/dx = (dx_u * w, dx_v * h)
        //   d(texel)/dy = (dy_u * w, dy_v * h)
        const double j00 = dx_u * static_cast<double>(base_width);
        const double j10 = dx_v * static_cast<double>(base_height);
        const double j01 = dy_u * static_cast<double>(base_width);
        const double j11 = dy_v * static_cast<double>(base_height);

        // singular values of J; rho is the larger one
        const double a = j00 * j00 + j10 * j10; // J^T J (0, 0)
        const double b = j00 * j01 + j10 * j11; // J^T J (0, 1)
        const double d = j01 * j01 + j11 * j11; // J^T J (1, 1)

        const double half_trace = 0.5 * (a + d);
        const double root = std::sqrt(std::max(0.0, 0.25 * (a - d) * (a - d) + b * b));
        const double lambda_max = std::max(0.0, half_trace + root);
        const double rho = std::sqrt(lambda_max);

        normal_moment_footprint f;
        f.rho = rho;
        f.lod_raw = std::log2(std::max(1.0, rho));

        const double last = static_cast<double>(levels.size() - 1);
        f.lod = std::min(last, std::max(0.0, f.lod_raw));

        f.level0 = static_cast<int>(floor(f.lod));
        if (f.level0 >= static_cast<int>(last)) {
            f.level0 = static_cast<int>(last);
            f.level1 = f.level0;
            f.weight1 = 0.0;
        } else {
            f.level1 = f.level0 + 1;
            f.weight1 = f.lod - static_cast<double>(f.level0);
        }

        return f;
    }

    normal_moment_moments normal_moments::sample(double u, double v,
                                                 double du_u, double du_v,
                                                 double dv_u, double dv_v,
                                                 normal_moment_footprint* footprint_out) const
    {
        const normal_moment_footprint f = footprint(du_u, du_v, dv_u, dv_v);
        if (footprint_out) {
            *footprint_out = f;
        }

        // blend the five moments across the (at most two) selected levels
        const normal_moment_moments m0 = sample_level(static_cast<unsigned>(f.level0), u, v);
        if (f.level1 == f.level0 || f.weight1 == 0.0) {
            return m0;
        }

        const normal_moment_moments m1 = sample_level(static_cast<unsigned>(f.level1), u, v);
        return m0 * (1.0 - f.weight1) + m1 * f.weight1;
    }
}
