#include "tgaimage.h"
#include "geometry.h"

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

// Shared rasterization math, used by both the legacy shader path and the sparse
// virtual texture path. The legacy rasterize() below is just one consumer of
// these helpers, so the two paths never re-implement coverage/barycentrics/depth.
struct ProjectedTriangle {
    vec2    screen[3]; // screen-space vertices
    vec4    ndc[3];    // normalized device coordinates (clip / w)
    mat<3,3> ABC;      // screen-space barycentric matrix
};

// Clip-to-screen projection. Returns false for back-facing / sub-pixel triangles
// (determinant < 1), exactly matching the legacy rasterize() culling rule.
bool project_triangle(const vec4 clip[3], const mat<4,4> &viewport, ProjectedTriangle &out);

// Screen-space barycentric coordinates of pixel (x, y); components may be
// negative, a negative component means the pixel is outside the triangle.
vec3 screen_barycentric(const mat<3,3> &ABC, const double x, const double y);

// Perspective-correct barycentrics: screen barycentrics divided by the clip-space
// w of each vertex and renormalized.
vec3 perspective_barycentric(const vec3 &bc_screen, const vec4 clip[3]);

// NDC depth, linearly interpolated with the screen-space barycentrics.
double interpolate_depth(const vec4 ndc[3], const vec3 &bc_screen);
