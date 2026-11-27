// Programmatic transparency demo: two intersecting colored thin sheets, an
// opaque face that hides part of them, and a checkerboard background.
// Usage: transparency_example output.tga
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "our_gl.h"
#include "transparency.h"

namespace {

constexpr int W = 320;
constexpr int H = 320;

struct SolidShader : IShader {
    TGAColor color;
    explicit SolidShader(TGAColor c) : color(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, color}; }
};

struct SheetShader : ITransparentShader {
    vec3 rgb;
    double alpha;
    SheetShader(vec3 c, double a) : rgb(c), alpha(a) {}
    TransparentFragment fragment(vec3) const override {
        TransparentFragment f;
        f.rgb = rgb;
        f.alpha = alpha;
        return f;
    }
};

// Make sure the screen-space winding survives the renderer's backface culling,
// and hand the primitive to the shared raster setup.
void orient(Triangle &t) {
    vec4 n0 = t[0]/t[0].w, n1 = t[1]/t[1].w, n2 = t[2]/t[2].w;
    vec2 s0 = (Viewport*n0).xy(), s1 = (Viewport*n1).xy(), s2 = (Viewport*n2).xy();
    double det = (s1.x-s0.x)*(s2.y-s0.y) - (s2.x-s0.x)*(s1.y-s0.y);
    if (det < 0) std::swap(t[1], t[2]);
}

constexpr double PI = 3.14159265358979323846;

vec3 rotate_y(const vec3 &p, double angle) {
    return { std::cos(angle)*p.x + std::sin(angle)*p.z, p.y,
            -std::sin(angle)*p.x + std::cos(angle)*p.z };
}

vec4 project(const vec3 &p) {
    return Perspective * (ModelView * vec4{p.x, p.y, p.z, 1.});
}

TGAColor checker(int x, int y) {
    const bool light = ((x/16) + (y/16)) % 2 == 0;
    TGAColor c;
    c.bytespp = 3;
    c[0] = light ? 70 : 140;   // B
    c[1] = light ? 90 : 120;   // G
    c[2] = light ? 120 : 45;   // R
    return c;
}

// Submit a rectangle (two triangles) spanning local corners with the given
// rotation about the Y axis.
void submit_sheet(TransparencyBuffer &tb, const SheetShader &shader,
                  vec3 a, vec3 b, vec3 c, vec3 d, double angle) {
    vec3 q[4] = {rotate_y(a, angle), rotate_y(b, angle),
                 rotate_y(c, angle), rotate_y(d, angle)};
    const int indices[2][3] = {{0,1,2}, {0,2,3}};
    for (const auto &idx : indices) {
        Triangle t = { project(q[idx[0]]), project(q[idx[1]]), project(q[idx[2]]) };
        orient(t);
        tb.triangle(t, shader);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " output.tga\n";
        return 1;
    }

    constexpr vec3 eye{0, 0, 3}, center{0, 0, 0}, up{0, 1, 0};
    lookat(eye, center, up);
    init_perspective(norm(eye-center));
    init_viewport(0, 0, W, H);
    init_zbuffer(W, H);

    TGAImage framebuffer(W, H, TGAImage::RGB);
    for (int y=0; y<H; y++)
        for (int x=0; x<W; x++)
            framebuffer.set(x, y, checker(x, y));

    // Opaque face in front of the sheets: drawn first, so its depth blocks the
    // transparent fragments behind it.
    {
        SolidShader shader(TGAColor{70, 70, 80, 255, 4});
        Triangle t = { project({-1.6, 0.0, 0.9}),
                       project({ 0.2, 0.0, 0.9}),
                       project({ 0.2, 1.4, 0.9}) };
        orient(t);
        rasterize(t, shader, framebuffer);
        Triangle t2 = { project({-1.6, 0.0, 0.9}),
                        project({ 0.2, 1.4, 0.9}),
                        project({-1.6, 1.4, 0.9}) };
        orient(t2);
        rasterize(t2, shader, framebuffer);
    }

    // Transparent sheets. They cross each other, so a depth buffer alone would
    // hide one of them; weighted blending handles the intersection regardless
    // of submission order.
    TransparencyBuffer transparency(W, H);
    transparency.clear();
    transparency.set_opaque_depth(zbuffer);
    transparency.submit();

    const double s = 1.15;
    SheetShader red({0.9, 0.08, 0.06}, 0.5);
    submit_sheet(transparency, red, {-s,-s,0.1}, {s,-s,0.1}, {s,s,0.1}, {-s,s,0.1},  PI/5);
    SheetShader blue({0.08, 0.2, 0.9}, 0.5);
    submit_sheet(transparency, blue, {-s,-s,-0.1}, {s,-s,-0.1}, {s,s,-0.1}, {-s,s,-0.1}, -PI/5);

    transparency.composite(framebuffer);

    if (!framebuffer.write_tga_file(argv[1])) {
        std::cerr << "failed to write " << argv[1] << "\n";
        return 1;
    }
    std::cout << "wrote " << argv[1] << "\n";
    return 0;
}
