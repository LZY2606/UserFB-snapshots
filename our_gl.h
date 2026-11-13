#pragma once
#include "tgaimage.h"
#include "geometry.h"
#include <vector>

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
    virtual ~IShader() = default;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

// ---------------------------------------------------------------------------
// Shadow-volume rendering path (z-fail, hard directional shadows).
//
// Clip convention used by every entry point below: a point with clip
// coordinates (x,y,z,w) is inside the visible volume when w > 0 and
// -w <= x,y,z <= w. The near plane is z = +w, the far plane z = -w, and a
// larger z/w is closer. Both orthographic and perspective world-to-clip
// matrices work.
//
// Support range: shadow-volume geometry may cross the near/side/eye planes,
// but all of its vertices are required to satisfy z >= -w, so the far cap is
// never clipped and no far-plane capping is performed. Receivers are clipped
// against all six planes.
//
// Sampling: pixel (x,y) covers [x,x+1) x [y,y+1) with y pointing up; every
// sample is taken at the half-pixel center (x+0.5, y+0.5). Projected
// triangles are first oriented to positive area; downward edges and leftward
// horizontal edges own their boundary, so clipping-fan inner edges and shared
// mesh edges cover every sample exactly once.
// ---------------------------------------------------------------------------

struct SurfaceVaryings {
    vec2 uv;          // texture coordinates
    vec4 nrm;         // eye-space vertex normal
    vec4 view;        // eye-space position
};

struct SurfaceFragment {
    bool discard = false;
    vec3 albedo;      // unquantized linear color (0..255 scale)
    double ambient = 0;
    double direct  = 0; // diffuse + specular, kept separate until composition
};

class ISurfaceShader {
public:
    virtual ~ISurfaceShader() = default;
    virtual SurfaceVaryings vertex(const int face, const int nthvert, vec4 &clip) = 0;
    // `corners` are the three original vertex records; `bar` are their
    // perspective-correct barycentric weights at the sample point.
    virtual SurfaceFragment fragment(const SurfaceVaryings corners[3],
                                     const vec3 &bar) const = 0;
};

class ShadowVolume;

class ShadowRenderer {
public:
    ShadowRenderer(const int width, const int height, const mat<4,4> &viewport);
    ~ShadowRenderer();

    int width()  const;
    int height() const;

    // Explicit per-frame reset of depth, validity flags, lighting records and
    // the signed stencil. All buffers always have matching dimensions.
    void clear();
    // Reset only the stencil, e.g. before evaluating another light. Drawing
    // another shadow pass without this call would wrongly accumulate.
    void clear_stencil();

    // Opaque receiver/occluder pass: records visible depth, validity and
    // unquantized lighting. Discarded fragments write none of these.
    void draw_surface(const int nfaces, ISurfaceShader &shader);
    // One closed shadow volume in the z-fail style: no color/depth writes;
    // depth-failed fragments decrement (front faces) / increment (back faces)
    // the signed integer stencil, including fragments at equal depth.
    void draw_volume(const ShadowVolume &volume, const mat<4,4> &world2clip);

    // Final composition: ambient + visibility*(diffuse+specular), clamped and
    // quantized only here. Pixels without a visible surface keep `background`.
    void compose(TGAImage &framebuffer, const TGAColor &background) const;

    // Read-only inspection for tests and the example.
    const std::vector<double>&       depth_buffer()  const;
    const std::vector<unsigned char>& valid_buffer() const;
    const std::vector<vec3>&         albedo_buffer() const;
    const std::vector<double>&       ambient_buffer() const;
    const std::vector<double>&       direct_buffer() const;
    const std::vector<int>&          stencil_buffer() const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};
