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
        // points carry no 2D attribute plane
        clear_gradient_plane();

        // whether interpolation is enabled or not, point is rendered the same in both cases
        if (!interp_enabled)
        {
            provoking_depth = d;
            provoking_attribs = v;
        }

        process_fragment(x, y, d, *v);
    }

    void rasterizer::fill_line(int x1, int y1, float d1, const vector<float>* v1,
                               int x2, int y2, float d2, const vector<float>* v2)
    {
        // lines carry no 2D attribute plane
        clear_gradient_plane();

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
            process_fragment(fb_x, fb_y, interp_depth[0], interp_attribs[0]);

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
            clear_gradient_plane(); // first-vertex attributes, zero gradients
        }
        else if (fs->wants_gradients())
        {
            // one affine plane through the three integer-quantized canvas vertices,
            // built before any vertex reordering so it follows the submitted triangle
            init_gradient_plane(x1, y1, v1, x2, y2, v2, x3, y3, v3);
        }
        else
        {
            clear_gradient_plane();
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
                process_fragment(fb_x, fb_y, interp_depth[2], interp_attribs[2]);
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
                process_fragment(fb_x, fb_y, interp_depth[2], interp_attribs[2]);
            }

            left_error -= 2 * left_dx;
            right_error -= 2 * right_dx;
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
            if (fs->wants_gradients())
            {
                if (gradient_active)
                {
                    // value, dx and dy all come from the same triangle affine plane,
                    // sampled at the covered integer canvas position
                    sample_gradient_plane(int(x), int(y));
                    fb.write(fb_index, fs->run(grad_value, grad_dx, grad_dy, true));
                }
                else
                {
                    // point, line or disabled interpolation: explicitly no 2D gradients
                    grad_zero.assign(final_v.size(), 0.f);
                    fb.write(fb_index, fs->run(final_v, grad_zero, grad_zero, false));
                }
            }
            else
            {
                const color fb_col = fs->run(final_v);
                fb.write(fb_index, fb_col);
            }
        }
    }

    void rasterizer::init_gradient_plane(int x1, int y1, const vector<float>* v1,
                                         int x2, int y2, const vector<float>* v2,
                                         int x3, int y3, const vector<float>* v3)
    {
        assert(v1->size() == v2->size() && v1->size() == v3->size());

        const unsigned n = unsigned(v1->size());
        grad_px = x1;
        grad_py = y1;
        grad_value0 = *v1;
        grad_dx.assign(n, 0.f);
        grad_dy.assign(n, 0.f);
        grad_value.assign(n, 0.f);
        grad_zero.assign(n, 0.f);

        // solve [dx12 dy12; dx13 dy13] * (gx, gy) = (v2 - v1, v3 - v1) per component
        const float dx12 = float(x2 - x1);
        const float dy12 = float(y2 - y1);
        const float dx13 = float(x3 - x1);
        const float dy13 = float(y3 - y1);
        const float det = dx12 * dy13 - dy12 * dx13;

        if (det != 0.f) // quantized triangles are guaranteed non-degenerate
        {
            const float inv_det = 1.f / det;
            for (unsigned i = 0; i < n; i++)
            {
                const float a = (*v2)[i] - (*v1)[i];
                const float b = (*v3)[i] - (*v1)[i];
                grad_dx[i] = (a * dy13 - dy12 * b) * inv_det;
                grad_dy[i] = (dx12 * b - a * dx13) * inv_det;
            }
        }

        gradient_active = true;
    }

    void rasterizer::clear_gradient_plane()
    {
        // keeps allocated storage but guarantees no stale triangle plane is visible
        gradient_active = false;
    }

    void rasterizer::sample_gradient_plane(int x, int y)
    {
        const float fx = float(x - grad_px);
        const float fy = float(y - grad_py);
        for (unsigned i = 0; i < grad_value.size(); i++)
        {
            grad_value[i] = grad_value0[i] + grad_dx[i] * fx + grad_dy[i] * fy;
        }
    }

    void rasterizer::set_interp_enabled(bool interp_enabled) {
        this->interp_enabled = interp_enabled;
    }

    void rasterizer::set_fs(const fragment_shader& fs) {
        this->fs = &fs;
    }
}
