// https://github.com/dominiksalvet/cpurast

#include "rasterizer.hpp"
#include <cmath>

using std::abs;
using std::swap;

namespace cr
{
    rasterizer::rasterizer(framebuf& fb, const fragment_shader& fs) :
        fb(fb),
        fs(&fs) {}

    void rasterizer::fill_point(int x, int y, float d, const vector<float>* v)
    {
        // whether interpolation is enabled or not, point is rendered the same in both cases
        if (!interp_enabled)
        {
            provoking_depth = d;
            provoking_attribs = v;
        }

        if (gradient_enabled) {
            process_fragment_gradient(x, y, d, *v, false);
        } else {
            process_fragment(x, y, d, *v);
        }
    }

    void rasterizer::fill_line(int x1, int y1, float d1, const vector<float>* v1,
                               int x2, int y2, float d2, const vector<float>* v2)
    {
        if (!interp_enabled)
        {
            provoking_depth = d1;
            provoking_attribs = v1;
        }

        // eliminate 2nd, 3rd, 6th, 7th octants (steep ones)
        bool steep = false;
        if (abs(y2 - y1) > abs(x2 - x1))
        {
            swap(x1, y1);
            swap(x2, y2);
            steep = true;
        }
        // eliminate 4th and 5th octants
        if (x1 > x2)
        {
            // swap vertices and all required data
            swap(x1, x2);
            swap(y1, y2);
            swap(d1, d2);
            swap(v1, v2);
        }
        // eliminate 8th octant
        const int y_step = y2 > y1 ? 1 : -1;

        // end point differences
        const int dx = x2 - x1;
        const int dy = abs(y2 - y1);
        int error = 2 * dy - dx;

        // initial coordinates
        int x = x1;
        int y = y1;
        // final framebuffer coordinates
        const int& fb_x = steep ? y : x;
        const int& fb_y = steep ? x : y;

        init_interpolation(0, *v1, dx);
        while (x <= x2) // from x1 to x2
        {
            interpolation(0, d1, *v1, d2, *v2, x - x1);
            if (gradient_enabled) {
                // lines have no two-dimensional gradient
                if (interp_enabled) {
                    process_fragment_gradient(fb_x, fb_y, interp_depth[0], interp_attribs[0], false);
                } else {
                    process_fragment_gradient(fb_x, fb_y, d1, *v1, false);
                }
            } else {
                process_fragment(fb_x, fb_y, interp_depth[0], interp_attribs[0]);
            }

            if (error > 0)
            {
                y += y_step;
                error -= 2 * dx;
            }
            error += 2 * dy;

            x++;
        }
    }

    void rasterizer::fill_triangle(int x1, int y1, float d1, const vector<float>* v1,
                                   int x2, int y2, float d2, const vector<float>* v2,
                                   int x3, int y3, float d3, const vector<float>* v3)
    {
        if (!interp_enabled)
        {
            provoking_depth = d1;
            provoking_attribs = v1;
        }

        if (gradient_enabled) {
            // the attribute planes go through the exact quantized triangle vertices
            // and are expressed in canvas pixels; this must not be replaced by the
            // discrete edge/span interpolation steps below
            prepare_triangle_planes(v1, v2, v3, x1, y1, x2, y2, x3, y3);
        }

        // sort vertices by its y position (so that y1 <= y2 <= y3)
        if (y1 > y2) {
            swap(x1, x2); swap(y1, y2); swap(d1, d2); swap(v1, v2);
        }
        if (y1 > y3) {
            swap(x1, x3); swap(y1, y3); swap(d1, d3); swap(v1, v3);
        }
        if (y2 > y3) {
            swap(x2, x3); swap(y2, y3); swap(d2, d3); swap(v2, v3);
        }

        // compute position differences
        const int dx12 = x2 - x1;
        const int dx13 = x3 - x1;
        const int dx23 = x3 - x2;
        const int dy12 = y2 - y1;
        const int dy13 = y3 - y1;
        const int dy23 = y3 - y2;

        // determine left and right edge
        const float slope12 = float(dx12) / dy12;
        const float slope13 = float(dx13) / dy13;
        const bool edge12_is_left = slope12 < slope13;

        // prepare left and right variables
        int left_dx, right_dx;
        int left_dy, right_dy;
        float left_d, right_d;
        const vector<float> *left_attribs, *right_attribs;

        // render top flat triangle (from bottom)
        if (edge12_is_left)
        {
            left_dx = dx12;    right_dx = dx13;
            left_dy = dy12;    right_dy = dy13;
            left_d = d2;       right_d = d3;
            left_attribs = v2; right_attribs = v3;
        }
        else
        {
            left_dx = dx13;    right_dx = dx12;
            left_dy = dy13;    right_dy = dy12;
            left_d = d3;       right_d = d2;
            left_attribs = v3; right_attribs = v2;
        }

        int left_dx_sign = left_dx < 0 ? -1 : 1;
        int right_dx_sign = right_dx < 0 ? -1 : 1;
        left_dx = abs(left_dx);
        right_dx = abs(right_dx);

        int left_error = left_dy - left_dx;
        int right_error = right_dy - right_dx;
        int left_x = x1;
        int right_x = x1;

        init_interpolation(0, *v1, left_dx + left_dy);
        init_interpolation(1, *v1, right_dx + right_dy);
        for (int fb_y = y1; fb_y < y2; fb_y++)
        {
            while (left_error <= 0)
            {
                left_error += 2 * left_dy;
                left_x += left_dx_sign;
            }
            while (right_error <= 0)
            {
                right_error += 2 * right_dy;
                right_x += right_dx_sign;
            }

            interpolation(0, d1, *v1, left_d, *left_attribs, abs(x1 - left_x) + fb_y - y1);
            interpolation(1, d1, *v1, right_d, *right_attribs, abs(x1 - right_x) + fb_y - y1);

            init_interpolation(2, *v1, right_x - left_x);
            for (int fb_x = left_x; fb_x < right_x; fb_x++)
            {
                interpolation(2, interp_depth[0], interp_attribs[0], interp_depth[1], interp_attribs[1], fb_x - left_x);
                if (gradient_enabled) {
                    process_fragment_gradient(fb_x, fb_y, interp_depth[2], interp_attribs[2], true);
                } else {
                    process_fragment(fb_x, fb_y, interp_depth[2], interp_attribs[2]);
                }
            }

            left_error -= 2 * left_dx;
            right_error -= 2 * right_dx;
        }

        // render bottom flat triangle (from top)
        if (edge12_is_left)
        {
            left_dx = dx23;
            left_dy = dy23;
            right_d = d1;
            right_attribs = v1;

            left_dx_sign = left_dx < 0 ? -1 : 1;
            left_dx = abs(left_dx);
        }
        else
        {
            right_dx = dx23;
            right_dy = dy23;
            left_d = d1;
            left_attribs = v1;

            right_dx_sign = right_dx < 0 ? -1 : 1;
            right_dx = abs(right_dx);
        }

        left_error = left_dy - left_dx;
        right_error = right_dy - right_dx;
        left_x = x3;
        right_x = x3;

        init_interpolation(0, *v3, left_dx + left_dy);
        init_interpolation(1, *v3, right_dx + right_dy);
        for (int fb_y = y3 - 1; fb_y >= y2; fb_y--)
        {
            while (left_error < 0)
            {
                left_error += 2 * left_dy;
                left_x -= left_dx_sign;
            }
            while (right_error < 0)
            {
                right_error += 2 * right_dy;
                right_x -= right_dx_sign;
            }

            interpolation(0, d3, *v3, left_d, *left_attribs, abs(x3 - left_x) + y3 - fb_y);
            interpolation(1, d3, *v3, right_d, *right_attribs, abs(x3 - right_x) + y3 - fb_y);

            init_interpolation(2, *v1, right_x - left_x);
            for (int fb_x = left_x; fb_x < right_x; fb_x++)
            {
                interpolation(2, interp_depth[0], interp_attribs[0], interp_depth[1], interp_attribs[1], fb_x - left_x);
                if (gradient_enabled) {
                    process_fragment_gradient(fb_x, fb_y, interp_depth[2], interp_attribs[2], true);
                } else {
                    process_fragment(fb_x, fb_y, interp_depth[2], interp_attribs[2]);
                }
            }

            left_error -= 2 * left_dx;
            right_error -= 2 * right_dx;
        }
    }

    void rasterizer::prepare_triangle_planes(const vector<float>* v1, const vector<float>* v2,
                                             const vector<float>* v3,
                                             int i1x, int i1y, int i2x, int i2y, int i3x, int i3y)
    {
        assert(v1->size() == v2->size() && v1->size() == v3->size());

        const double x1 = i1x, y1 = i1y;
        const double x2 = i2x, y2 = i2y;
        const double x3 = i3x, y3 = i3y;
        const double det = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);

        // the reference point is the first submitted vertex, which is on the plane
        plane_ref_x = i1x;
        plane_ref_y = i1y;
        plane_ref_value = *v1;

        const size_t count = v1->size();
        plane_dx.assign(count, 0.f);
        plane_dy.assign(count, 0.f);

        if (det == 0.0) {
            return; // degenerate triangle: constant attributes, zero derivatives
        }

        for (size_t i = 0; i < count; i++)
        {
            const double dv2 = double((*v2)[i]) - double((*v1)[i]);
            const double dv3 = double((*v3)[i]) - double((*v1)[i]);

            // solve [x2-x1, y2-y1; x3-x1, y3-y1] * [gx, gy] = [dv2, dv3]
            plane_dx[i] = float((dv2 * (y3 - y1) - dv3 * (y2 - y1)) / det);
            plane_dy[i] = float((dv3 * (x2 - x1) - dv2 * (x3 - x1)) / det);
        }
    }

    void rasterizer::init_interpolation(unsigned index, const vector<float>& v1, unsigned total_steps)
    {
        if (!interp_enabled) {
            return;
        }

        assert(index <= 3);

        interp_step[index] = total_steps == 0 ? 1.f : 1.f / total_steps;
        interp_attribs[index].resize(v1.size());
    }

    void rasterizer::interpolation(unsigned index, float d1, const vector<float>& v1, float d2, const vector<float>& v2, unsigned cur_step)
    {
        if (!interp_enabled) {
            return;
        }

        assert(index <= 3);
        assert(v1.size() == v2.size());

        const float weight2 = cur_step * interp_step[index];
        const float weight1 = 1.f - weight2;

        assert(weight2 >= 0.f && weight2 <= 1.f);

        interp_depth[index] = weight1 * d1 + weight2 * d2; // depth interpolation

        for (unsigned i = 0; i < interp_attribs[index].size(); i++)
        {
            // interpolation of attributes
            interp_attribs[index][i] = weight1 * v1[i] + weight2 * v2[i];
        }
    }

    void rasterizer::process_fragment(unsigned x, unsigned y, float d, const vector<float>& v)
    {
        // select first vertex attributes if interpolation is not enabled (and performed)
        const float& final_d = interp_enabled ? d : provoking_depth;
        const vector<float>& final_v = interp_enabled ? v : *provoking_attribs;

        // compute final framebuffer index
        const unsigned fb_index = fb.get_index(x, y);

        // depth test -> fragment shader -> write to framebuffer
        if (fb.depth_test(fb_index, final_d)) // early depth test before running fragment shader
        {
            const color fb_col = fs->run(final_v);
            fb.write(fb_index, fb_col);
        }
    }

    void rasterizer::process_fragment_gradient(unsigned x, unsigned y, float d, const vector<float>& v,
                                               bool triangle_with_2d)
    {
        // points, lines, and disabled interpolation explicitly have no 2D gradient;
        // never read leftover planes from a previously rendered triangle in that case
        const bool has_2d = triangle_with_2d && interp_enabled;

        const float& final_d = interp_enabled ? d : provoking_depth;
        const vector<float>& final_v = interp_enabled ? v : *provoking_attribs;

        const gradient_fragment_shader* gfs = dynamic_cast<const gradient_fragment_shader*>(fs);

        if (!gfs) {
            // old shader programs keep their old inputs and old outputs
            process_fragment(x, y, d, v);
            return;
        }

        if (has_2d) {
            // evaluate the common affine attribute plane at this canvas pixel
            plane_value.resize(plane_ref_value.size());
            const long long dx = static_cast<long long>(x) - static_cast<long long>(plane_ref_x);
            const long long dy = static_cast<long long>(y) - static_cast<long long>(plane_ref_y);
            for (size_t i = 0; i < plane_value.size(); i++)
            {
                plane_value[i] = plane_ref_value[i] + plane_dx[i] * float(dx) + plane_dy[i] * float(dy);
            }

            const fragment_gradient grad{plane_value, plane_dx, plane_dy, true, x, y};
            const unsigned fb_index = fb.get_index(x, y);
            if (fb.depth_test(fb_index, final_d)) {
                const color fb_col = gfs->run_gradient(grad);
                fb.write(fb_index, fb_col);
            }
            return;
        }

        // no two-dimensional gradient: zero derivatives and the provoking attribute
        grad_dx.assign(final_v.size(), 0.f);
        grad_dy.assign(final_v.size(), 0.f);

        const fragment_gradient grad{final_v, grad_dx, grad_dy, false, x, y};
        const unsigned fb_index = fb.get_index(x, y);
        if (fb.depth_test(fb_index, final_d)) {
            const color fb_col = gfs->run_gradient(grad);
            fb.write(fb_index, fb_col);
        }
    }

    void rasterizer::set_interp_enabled(bool interp_enabled) {
        this->interp_enabled = interp_enabled;
    }

    void rasterizer::set_gradient_enabled(bool gradient_enabled) {
        this->gradient_enabled = gradient_enabled;
    }

    void rasterizer::set_fs(const fragment_shader& fs) {
        this->fs = &fs;
    }
}
