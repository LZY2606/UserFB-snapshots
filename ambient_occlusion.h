#pragma once
#include "geometry.h"
#include "tgaimage.h"
#include <vector>

// Screen-space ambient occlusion for the tinyrenderer CPU pipeline.
//
// View-space convention (matches our_gl.cpp): the camera sits at (0,0,f) in view
// coordinates, f = norm(eye-center), and looks toward -z. Visible surfaces
// have view z < f; the frontal depth of a point is f - z. All quantities
// (view positions, AO radius R, bias b) share the same world/view units.
// The non-linear clip-space z/w is never used as a view-space distance.

// Sink receiving per-pixel geometry from the rasterizer (see our_gl.cpp).
// Only fragments that pass the depth test and are not discarded by the
// fragment shader are recorded, so the geometry always belongs to the same
// surface that produced the final visible color of the pixel.
struct IGBufferSink {
    virtual void record(int x, int y, const vec3 &view_pos, const vec3 &view_nrm) = 0;
    virtual ~IGBufferSink() = default;
};

// Geometry buffer: one entry per pixel, sized with the framebuffer.
struct GBuffer : IGBufferSink {
    struct Entry {
        vec3 pos = {};      // view-space position of the visible surface
        vec3 nrm = {0,0,1}; // unit view-space normal actually used for lighting
        bool valid = false; // false = background, does not participate in AO
    };
    std::vector<Entry> data = {};
    int w = 0, h = 0;

    GBuffer() = default;
    GBuffer(const int width, const int height) { resize(width, height); }

    void resize(const int width, const int height); // (re)allocates and clears
    void clear();                                   // marks every pixel invalid
    void record(const int x, const int y, const vec3 &view_pos, const vec3 &view_nrm) override;

          Entry* at(const int x, const int y);      // nullptr when out of bounds
    const Entry* at(const int x, const int y) const;
    int width()  const { return w; }
    int height() const { return h; }
};

struct AOParams {
    double radius = 1.0;    // view-space sample radius R, must be finite and > 0
    double bias   = 0.0;    // frontal-depth bias b, must be finite and >= 0
    int filter_radius = 0;  // bilateral filter half-width in pixels, 0 disables it
};

bool ao_params_valid(const AOParams &p); // finite, R>0, b>=0, filter_radius>=0

// Deterministic 16-sample local hemisphere pattern, j=0..3, k=0..3:
//   z = (j+.5)/4, azimuth = 2*pi*(k + .5*(j%2))/4,
//   transverse length sqrt(1-z*z), sample distance R*(j+1)/4.
constexpr int AO_SAMPLE_COUNT = 16;
vec3   ao_sample_dir(const int j, const int k);       // unit direction, local frame (z = normal)
double ao_sample_dist(const AOParams &p, const int j);

// Stable tangent frame for a unit normal: the coordinate axis with the
// smallest absolute dot product with n is used to build the tangent.
void ao_tangent_frame(const vec3 &n, vec3 &t, vec3 &b);

// Raw ambient occlusion per pixel, in [0,1]; background pixels get 1.
// Samples projecting outside the screen, onto background, or behind the
// camera count as unoccluded. Returns false on invalid parameters.
bool compute_ao(const GBuffer &gb, const AOParams &params, std::vector<double> &ao_out);

// Optional separable bilateral filter of the raw AO (in place).
//   spatial weight  exp(-d*d/8),           d = pixel offset
//   depth weight    exp(-dz*dz/(2*(R/4)^2)), dz = frontal-depth difference
//   normal weight   max(0, dot(n0,n1))^8
// Background pixels are skipped as neighbors; the result is normalized by
// the sum of effective weights. Both passes read the geometry from the
// original gbuffer, so dark edges do not bleed across depth discontinuities.
// filter_radius==0 leaves the input untouched.
bool filter_ao(const GBuffer &gb, const AOParams &params, std::vector<double> &ao);

// Grayscale visualization of an AO buffer (1 -> white, 0 -> black).
TGAImage ao_grayscale(const std::vector<double> &ao, const int w, const int h);
