#pragma once
#include <vector>
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

// Optional per-fragment ownership. Color, depth and the two ownership tags are
// updated together, and only on the single fragment that both passes the
// "greater depth is closer" test and is not discarded by the fragment shader.
struct FragmentOwnership {
    std::vector<int> object; // object number, -1 when no surface owns the pixel
    std::vector<int> face;   // original face number inside the object, -1 when none
};

void clear_ownership(FragmentOwnership &owner);                       // fill both tags with -1
void resize_ownership(FragmentOwnership &owner, int width, int height); // (re)allocate and clear
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer,
               int object_id, int face_id, FragmentOwnership *owner);
