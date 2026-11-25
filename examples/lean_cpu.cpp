// https://github.com/dominiksalvet/cpurast

// lean_cpu - oblique view of a crossed-slope normal-moment material
//
// Renders a single slanted quad with the lean slope-moment shader and writes a
// P3 PPM to the path given as the first argument.
//
// The "lean" normal texture is generated procedurally: crossed slope ridges
// whose normals tilt in four quadrants around a mostly-forward base normal.

#include "cpurast.hpp"
#include "lean_shader.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    struct memory_canvas : cr::canvas
    {
        std::vector<cr::color> pixels;
        void draw(const std::vector<cr::color>& p, unsigned w, unsigned h) override
        {
            if (p.size() != size_t(w) * h) std::abort();
            pixels = p;
        }
    };

    // procedurally generated crossed-slope normal map (nx, ny, nz), row major
    std::vector<float> make_crossed_normals(unsigned w, unsigned h)
    {
        std::vector<float> n(size_t(w) * h * 3);
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
                const double u = (double(x) + 0.5) / w;
                const double v = (double(y) + 0.5) / h;
                // a fine diagonal weave plus a coarser cross
                const double p = 0.55 * std::sin(2.0 * M_PI * 3.0 * u) +
                                 0.30 * std::sin(2.0 * M_PI * 2.0 * v + 0.7) +
                                 0.18 * std::sin(2.0 * M_PI * (5.0 * u + 4.0 * v));
                const double q = 0.50 * std::sin(2.0 * M_PI * 3.0 * v + 1.1) +
                                 0.30 * std::sin(2.0 * M_PI * 2.0 * u + 0.2) +
                                 0.18 * std::sin(2.0 * M_PI * (4.0 * u - 5.0 * v));
                const double scale = 0.75;
                const double nz = 1.0;
                const double nx = scale * p * nz;
                const double ny = scale * q * nz;
                size_t i = (size_t(y) * w + x) * 3;
                n[i + 0] = float(nx);
                n[i + 1] = float(ny);
                n[i + 2] = float(nz);
            }
        }
        return n;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
        return 1;
    }

    const unsigned width = 480, height = 360;

    // ---- owning normal-moment texture (procedural source array is dropped) ----
    const unsigned tex_w = 96, tex_h = 96;
    std::vector<float> normals = make_crossed_normals(tex_w, tex_h);
    auto moments = std::make_shared<cr::normal_moments>(normals, tex_w, tex_h);
    normals.clear();
    normals.shrink_to_fit();

    // ---- material: diffuse, bluish specular, moderately tight highlight ----
    cr::lean_material mat;
    mat.diffuse[0] = 0.22f;
    mat.diffuse[1] = 0.26f;
    mat.diffuse[2] = 0.34f;
    mat.specular[0] = 0.75f;
    mat.specular[1] = 0.90f;
    mat.specular[2] = 1.00f;
    mat.shininess = 40.f;
    mat.light[0] = -0.35f;
    mat.light[1] = 0.55f;
    mat.light[2] = 0.75f;

    memory_canvas canvas;
    cr::context ctx(canvas, width, height);
    ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
    ctx.bind_fragment_shader(std::make_shared<cr::lean_fragment_shader>(moments, mat));
    ctx.enable_fragment_gradients();
    ctx.enable_depth_test();
    ctx.clear_framebuf(true, true);

    // ---- an obliquely viewed quad in view space, split into two triangles ----
    // The quad lies roughly at z = -3, tilted about both axes so the view is
    // oblique; w = 1 keeps the restored attributes exact.
    const float z = -3.0f;
    const float corners[4][3] = {
        {-1.35f * 1.6f, -1.0f * 1.6f, z - 0.55f},
        { 1.35f * 1.6f, -1.0f * 1.6f, z + 0.55f},
        { 1.35f * 1.6f,  1.0f * 1.6f, z + 0.60f},
        {-1.35f * 1.6f,  1.0f * 1.6f, z - 0.50f}
    };
    const float uvs[4][2] = {{0.f, 0.f}, {2.f, 0.f}, {2.f, 3.f}, {0.f, 3.f}};

    // normals follow the tilt of the quad so lighting and B vary across it
    const float nrm[4][3] = {
        {-0.25f, 0.15f, 1.0f},
        { 0.20f, 0.10f, 1.0f},
        { 0.15f, -0.20f, 1.0f},
        {-0.10f, -0.15f, 1.0f}
    };
    const float tan[4][3] = {
        {1.0f, 0.0f, 0.15f},
        {0.9f, 0.1f, 0.2f},
        {0.8f, -0.1f, 0.25f},
        {0.95f, -0.05f, 0.18f}
    };

    auto clip_for = [](const float view[3]) -> std::array<float, 4> {
        // x/y stay inside normalized device coordinates, z is kept near the
        // middle of the depth range; w = 1
        return {view[0] / 2.6f, view[1] / 2.6f, view[2] / 6.0f, 1.f};
    };

    // packing helper: build the four packed inputs
    std::vector<float> packed[4];
    for (unsigned i = 0; i < 4; i++) {
        const std::array<float, 4> clip = clip_for(corners[i]);
        packed[i] = cr::lean::pack_vertex(clip.data(), corners[i], nrm[i], tan[i], 1.f,
                                          uvs[i][0], uvs[i][1]);
    }

    // two triangles of the quad
    ctx.draw_triangle(packed[0], packed[1], packed[2]);
    ctx.draw_triangle(packed[0], packed[2], packed[3]);
    ctx.update_canvas();

    // ---- write P3 PPM ----
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s for writing\n", argv[1]);
        return 1;
    }
    std::fprintf(f, "P3\n%u %u\n255\n", width, height);
    for (unsigned row = 0; row < height; row++) {
        const unsigned y = height - 1 - row; // color buffer has y=0 at the bottom
        for (unsigned x = 0; x < width; x++) {
            const cr::color& c = canvas.pixels[size_t(y) * width + x];
            const int r = int(std::lround(std::fmin(1.0, std::fmax(0.0, c.r)) * 255.0));
            const int g = int(std::lround(std::fmin(1.0, std::fmax(0.0, c.g)) * 255.0));
            const int b = int(std::lround(std::fmin(1.0, std::fmax(0.0, c.b)) * 255.0));
            std::fprintf(f, "%d %d %d\n", r, g, b);
        }
    }
    std::fclose(f);

    std::printf("LEAN_CPU written=%s %ux%u\n", argv[1], width, height);
    return 0;
}
