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
        // when gradient callbacks are enabled, the plane is built from the exact integer
        // triangle (x1, y1), (x2, y2), (x3, y3) as delivered to this function
        void fill_triangle(int x1, int y1, float d1, const vector<float>* v1,
                           int x2, int y2, float d2, const vector<float>* v2,
                           int x3, int y3, float d3, const vector<float>* v3);

        void set_interp_enabled(bool interp_enabled);
        // enable/disable optional gradient callbacks (a superset of attribute interpolation)
        void set_gradient_enabled(bool gradient_enabled);
        void set_fs(const fragment_shader& fs);

    private:
        // initialize interpolation (call once before a batch of interpolation calls)
        void init_interpolation(unsigned index, const vector<float>& v1, unsigned total_steps);
        // interpolates depths and attributes based on current step, uses member arrays based on index
        void interpolation(unsigned index, float d1, const vector<float>& v1, float d2, const vector<float>& v2, unsigned cur_step);
        // build affine attribute planes from the exact quantized triangle vertices
        void prepare_triangle_planes(const vector<float>* v1, const vector<float>* v2, const vector<float>* v3,
                                     int i1x, int i1y, int i2x, int i2y, int i3x, int i3y);
        // evaluate attribute plane and derivatives, then run the fragment shader
        void process_fragment(unsigned x, unsigned y, float d, const vector<float>& v);
        void process_fragment_gradient(unsigned x, unsigned y, float d, const vector<float>& v,
                                       bool triangle_with_2d);

        bool interp_enabled = true; // if interpolation is disabled, provoking attributes are used
        bool gradient_enabled = false; // if enabled, gradient fragment callbacks are used
        float provoking_depth;
        const vector<float>* provoking_attribs;

        float interp_step[3]; // prepared interpolation steps
        float interp_depth[3]; // interpolated depths
        vector<float> interp_attribs[3]; // interpolated attributes

        // affine attribute planes of the current triangle (per canvas pixel units)
        int plane_ref_x; // plane reference pixel
        int plane_ref_y;
        vector<float> plane_ref_value; // attribute value at the reference pixel
        vector<float> plane_value; // value at the current pixel
        vector<float> plane_dx; // d(value)/dx
        vector<float> plane_dy; // d(value)/dy

        // property values and derivatives of the currently processed fragment
        vector<float> grad_value;
        vector<float> grad_dx;
        vector<float> grad_dy;

        framebuf& fb;
        const fragment_shader* fs;
    };
}
