// lean mapping example: an obliquely viewed material with a procedurally generated
// cross-slope normal map, rendered without SDL. Writes a binary-free P3 PPM image to
// the path given as the first argument.

#include "cpurast.hpp"
#include "normal_moments.hpp"
#include "lean_shader.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace
{
    constexpr unsigned CANVAS_W = 120;
    constexpr unsigned CANVAS_H = 90;
    constexpr unsigned TEX_W = 64;
    constexpr unsigned TEX_H = 64;
    constexpr float FOCAL = 2.0f;
    constexpr float HALF = 1.5f;
    constexpr float DEPTH = 4.0f;
    constexpr float TILT = 0.6f; // radians, oblique view of the material

    struct memory_canvas : cr::canvas
    {
        std::vector<cr::color> pixels;
        void draw(const std::vector<cr::color>& p, unsigned w, unsigned h) override
        {
            if (p.size() != w * h) std::abort();
            pixels = p;
        }
    };

    // procedural cross-slope normal map: two crossing diagonal wave families
    std::vector<float> build_cross_slope_normals()
    {
        std::vector<float> normals(TEX_W * TEX_H * 3);
        const float amp = 0.9f;
        const float k = 6.0f * 2.0f * 3.14159265358979f;
        for (unsigned y = 0; y < TEX_H; y++)
        {
            for (unsigned x = 0; x < TEX_W; x++)
            {
                const float sx = float(x) / float(TEX_W - 1);
                const float sy = float(y) / float(TEX_H - 1);
                const float nx = amp * std::sin(k * (sx + sy));
                const float ny = amp * std::sin(k * (sx - sy));
                const float nz = 1.0f;
                const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
                normals[(y * TEX_W + x) * 3 + 0] = nx * inv;
                normals[(y * TEX_W + x) * 3 + 1] = ny * inv;
                normals[(y * TEX_W + x) * 3 + 2] = nz * inv;
            }
        }
        return normals;
    }

    // one corner of the tilted material quad in view space
    std::vector<float> quad_vertex(float lx, float ly, float u, float v)
    {
        const float ct = std::cos(TILT);
        const float st = std::sin(TILT);

        const float px = lx;
        const float py = ly * ct;
        const float pz = ly * st - DEPTH; // always negative: in front of the viewer

        // local normal (0,0,1) and tangent (1,0,0) rotated about the x axis
        const float nx = 0.f, ny = -st, nz = ct;
        const float tx = 1.f, ty = 0.f, tz = 0.f;

        const float w = -pz; // perspective w, always positive
        const float clip_x = FOCAL * px;
        const float clip_y = FOCAL * py;
        const float clip_z = 0.f;

        return cr::make_lean_vertex(clip_x, clip_y, clip_z, w,
                                    px, py, pz,
                                    nx, ny, nz,
                                    tx, ty, tz,
                                    1.f, u, v);
    }

    int to_byte(float x)
    {
        if (x < 0.f) x = 0.f;
        if (x > 1.f) x = 1.f;
        return int(x * 255.f + 0.5f);
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
        return 1;
    }

    std::vector<float> normal_data = build_cross_slope_normals();
    auto texture = std::make_shared<cr::normal_moments_texture>(TEX_W, TEX_H, normal_data);
    normal_data.clear(); // the texture owns its data, the source array is released

    memory_canvas canvas;
    cr::context ctx(canvas, CANVAS_W, CANVAS_H);

    auto vs = std::make_shared<cr::lean_vs>();
    auto fs = std::make_shared<cr::lean_fs>(texture);
    fs->set_material({0.15f, 0.18f, 0.22f}, // diffuse
                     {0.95f, 0.95f, 0.9f},  // specular
                     64.f,                  // shininess
                     {-0.4f, 0.5f, 0.77f}); // unit-ish direction towards the light
    ctx.bind_vertex_shader(vs);
    ctx.bind_fragment_shader(fs);
    ctx.enable_depth_test();
    ctx.clear_framebuf(true, true);

    const std::vector<float> v00 = quad_vertex(-HALF, -HALF, 0.f, 0.f);
    const std::vector<float> v10 = quad_vertex(HALF, -HALF, 1.f, 0.f);
    const std::vector<float> v11 = quad_vertex(HALF, HALF, 1.f, 1.f);
    const std::vector<float> v01 = quad_vertex(-HALF, HALF, 0.f, 1.f);

    ctx.draw_triangle(v00, v10, v11);
    ctx.draw_triangle(v00, v11, v01);
    ctx.update_canvas();

    std::ofstream out(argv[1]);
    if (!out)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    out << "P3\n" << CANVAS_W << ' ' << CANVAS_H << "\n255\n";
    for (unsigned y = 0; y < CANVAS_H; y++)
    {
        for (unsigned x = 0; x < CANVAS_W; x++)
        {
            // framebuffer origin is bottom-left, PPM is top-left
            const cr::color& c = canvas.pixels[(CANVAS_H - 1 - y) * CANVAS_W + x];
            out << to_byte(c.r) << ' ' << to_byte(c.g) << ' ' << to_byte(c.b) << '\n';
        }
    }
    return out.good() ? 0 : 1;
}
