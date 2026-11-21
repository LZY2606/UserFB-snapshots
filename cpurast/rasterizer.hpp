// https://github.com/dominiksalvet/cpurast

#pragma once

#include "framebuf.hpp"
#include "shader.hpp"

namespace cr
{
    class rasterizer
    {
    public:
        rasterizer(framebuf& fb, const fragment_shader& fs);

        // fill geometry primitives based on vertices position, depth and attributes
        void fill_point(int x, int y, float d, const vector<float>* v);
        // rasterization functions based on Bresenham's line algorithm (line and triangle)
        void fill_line(int x1, int y1, float d1, const vector<float>* v1,
                       int x2, int y2, float d2, const vector<float>* v2);
        // this function does not fill x=max(x1, x2, x3) nor y=max(y1, y2, y3) due to adjacent triangles support
        void fill_triangle(int x1, int y1, float d1, const vector<float>* v1,
                           int x2, int y2, float d2, const vector<float>* v2,
                           int x3, int y3, float d3, const vector<float>* v3);

        void set_interp_enabled(bool interp_enabled);
        void set_fs(const fragment_shader& fs);

    private:
        // initialize interpolation (call once before a batch of interpolation calls)
        void init_interpolation(unsigned index, const vector<float>& v1, unsigned total_steps);
        // interpolates depths and attributes based on current step, uses member arrays based on index
        void interpolation(unsigned index, float d1, const vector<float>& v1, float d2, const vector<float>& v2, unsigned cur_step);
        void process_fragment(unsigned x, unsigned y, float d, const vector<float>& v);

        // build the single affine attribute plane of a triangle through its three
        // integer-quantized canvas vertices; valid only for a non-degenerate triangle
        void init_gradient_plane(int x1, int y1, const vector<float>* v1,
                                 int x2, int y2, const vector<float>* v2,
                                 int x3, int y3, const vector<float>* v3);
        // drop any active triangle plane so points, lines and disabled interpolation
        // can never read derivatives left over from a previous triangle
        void clear_gradient_plane();
        // sample the active plane at integer canvas position (x, y) into grad_value
        void sample_gradient_plane(int x, int y);

        bool interp_enabled = true; // if interpolation is disabled, provoking attributes are used
        float provoking_depth;
        const vector<float>* provoking_attribs;

        float interp_step[3]; // prepared interpolation steps
        float interp_depth[3]; // interpolated depths
        vector<float> interp_attribs[3]; // interpolated attributes

        // affine attribute plane f(x, y) = grad_value0 + grad_dx * (x - px) + grad_dy * (y - py)
        // gradient_active is set only by a triangle and reset by every point/line draw
        bool gradient_active = false;
        int grad_px = 0, grad_py = 0;
        vector<float> grad_value0; // plane value at (grad_px, grad_py)
        vector<float> grad_dx; // per-canvas-pixel x derivatives
        vector<float> grad_dy; // per-canvas-pixel y derivatives
        vector<float> grad_value; // per-fragment plane value reused across fragments
        vector<float> grad_zero; // zero vectors handed out when no plane is active

        framebuf& fb;
        const fragment_shader* fs;
    };
}
