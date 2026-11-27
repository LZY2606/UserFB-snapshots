#pragma once

#include <utility>
#include <vector>

#include "tgaimage.h"
#include "geometry.h"

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

extern mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
extern std::vector<double> zbuffer;               // depth buffer

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points

// Shared triangle setup for both the opaque and the transparent raster paths.
// It performs the same NDC/viewport transform, backface culling, small-triangle
// rejection and screen-clipped bounding box computation as rasterize().
struct RasterTriangle {
    vec4 ndc[3] = {};
    vec2 screen[3] = {};
    mat<3,3> ABC = {};
    bool valid = false;             // false when the triangle is culled
    int bbminx = 0, bbmaxx = -1;    // bounding box clipped to the framebuffer
    int bbminy = 0, bbmaxy = -1;
};

RasterTriangle prepare_raster_triangle(const Triangle &clip, int width, int height);

// Per-pixel coverage test shared by rasterize() and the transparency path.
// Returns true when (x,y) lies inside the triangle; in that case bc_clip holds
// the perspective-correct barycentric coordinates and depth holds the ndc z
// (clip.z/clip.w), with the renderer convention "larger is nearer".
bool raster_coverage(const RasterTriangle &tri, const Triangle &clip,
                     int x, int y, vec3 &bc_clip, double &depth);

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

