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

        // build the affine attribute planes passing through the three quantized
        // integer triangle vertices; each component is value = a*x + b*y + c
        void init_gradient_planes(int x1, int y1, const vector<float>& v1,
                                  int x2, int y2, const vector<float>& v2,
                                  int x3, int y3, const vector<float>& v3);
        // evaluate the current affine planes at integer canvas coordinates (x, y)
        void evaluate_gradient_planes(int x, int y, vector<float>& v) const;
        // mark gradient information as unavailable (points, lines, disabled interpolation)
        void invalidate_gradients();

        bool interp_enabled = true; // if interpolation is disabled, provoking attributes are used
        float provoking_depth;
        const vector<float>* provoking_attribs;

        float interp_step[3]; // prepared interpolation steps
        float interp_depth[3]; // interpolated depths
        vector<float> interp_attribs[3]; // interpolated attributes

        // affine attribute planes of the triangle currently being rasterized
        bool gradients_valid = false; // no valid triangle plane until one is built
        vector<float> plane_a; // coefficient of canvas x, per attribute component
        vector<float> plane_b; // coefficient of canvas y, per attribute component
        vector<float> plane_c; // constant term, per attribute component
        vector<gradient> fragment_gradients; // dx/dy handed to the fragment shader
        vector<float> plane_attribs; // attributes evaluated from the planes

        framebuf& fb;
        const fragment_shader* fs;
    };
}
