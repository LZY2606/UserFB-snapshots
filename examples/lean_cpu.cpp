// lean material example: a procedurally generated cross-slope normal texture
// drawn on an obliquely viewed quad; writes a binary-free P3 PPM image
//
// usage: lean_cpu <output.ppm>
// no SDL, no downloaded packages, only C++17 and the standard library

#include "cpurast.hpp"
#include "normal_moments.hpp"
#include "lean_shader.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

namespace
{
    class memory_canvas : public cr::canvas
    {
    public:
        void draw(const std::vector<cr::color>& pixels, unsigned width, unsigned height) override
        {
            this->pixels = pixels;
            this->width = width;
            this->height = height;
        }

        std::vector<cr::color> pixels;
        unsigned width = 0;
        unsigned height = 0;
    };

    // criss-cross diagonal ridges: h = sin(k(x+y)) + sin(k(x-y))
    cr::normal_pixel cross_slope_normal(float u, float v)
    {
        constexpr float pi = 3.14159265358979323846f;
        constexpr float k = 3.f * pi; // crossing periods across the tile
        constexpr float amplitude = 0.06f; // gentle slopes, nz stays dominant

        const float sx = u - 0.5f;
        const float sy = v - 0.5f;
        const float dhdx = amplitude * k * (std::cos(k * (sx + sy)) + std::cos(k * (sx - sy)));
        const float dhdy = amplitude * k * (std::cos(k * (sx + sy)) - std::cos(k * (sx - sy)));

        // normal of the heightfield z = h(x, y), normalized
        const float nx = -dhdx;
        const float ny = -dhdy;
        const float nz = 1.f;
        const float inv_len = 1.f / std::sqrt(nx * nx + ny * ny + nz * nz);
        return {nx * inv_len, ny * inv_len, nz * inv_len};
    }

    // tracks the widest mip level any fragment actually reached
    struct lod_tracking_fs : cr::lean_fs
    {
        using cr::lean_fs::lean_fs;

        mutable float max_lod = 0.f;

        cr::color run(const std::vector<float>& in, const std::vector<cr::gradient>& g,
                      bool valid) const override
        {
            const cr::color col = cr::lean_fs::run(in, g, valid);
            const float lod = last_diagnostic().sample.lod;
            if (lod > max_lod) {
                max_lod = lod;
            }
            return col;
        }
    };

    void write_ppm(const char* path, const std::vector<cr::color>& pixels, unsigned width, unsigned height)
    {
        std::ofstream out(path, std::ios::trunc);
        out << "P3\n" << width << ' ' << height << "\n255\n";
        // framebuffer rows start at the bottom; PPM rows start at the top
        for (int y = static_cast<int>(height) - 1; y >= 0; y--)
        {
            for (unsigned x = 0; x < width; x++)
            {
                const cr::color& c = pixels[static_cast<size_t>(y) * width + x];
                const auto to_byte = [](float v) {
                    return static_cast<int>(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f);
                };
                out << to_byte(c.r) << ' ' << to_byte(c.g) << ' ' << to_byte(c.b) << '\n';
            }
        }
    }
}

int main(int argc, char* argv[])
{
    const char* out_path = argc > 1 ? argv[1] : "lean.ppm";

    constexpr unsigned tex_w = 32;
    constexpr unsigned tex_h = 32;
    std::vector<cr::normal_pixel> normals(static_cast<size_t>(tex_w) * tex_h);
    for (unsigned y = 0; y < tex_h; y++)
    {
        for (unsigned x = 0; x < tex_w; x++)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(tex_w);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(tex_h);
            normals[static_cast<size_t>(y) * tex_w + x] = cross_slope_normal(u, v);
        }
    }

    // the texture owns its data; both shaders share this single resource
    auto texture = std::make_shared<cr::normal_moments_texture>(normals, tex_w, tex_h);

    // a canvas smaller than the texture footprint so minification actually
    // reaches several mip levels instead of always sampling the base level
    constexpr unsigned canvas_w = 48;
    constexpr unsigned canvas_h = 36;
    memory_canvas canvas;
    cr::context ctx(canvas, canvas_w, canvas_h);

    auto vs = std::make_shared<cr::lean_vs>();
    cr::lean_material material;
    material.diffuse = {0.05f, 0.12f, 0.22f};
    material.specular = {0.95f, 0.95f, 1.f};
    material.shininess = 16.f;
    material.light_dir = {-0.30f, 0.35f, 0.89f}; // will be normalized inside the shader
    auto fs = std::make_shared<lod_tracking_fs>(texture, material);
    ctx.bind_vertex_shader(vs);
    ctx.bind_fragment_shader(fs);

    ctx.set_clear_color(0.02f, 0.02f, 0.03f);
    ctx.clear_framebuf(true, true);
    ctx.enable_depth_test();

    // quad rotated around the view-space y axis, so w differs between its
    // left and right edges and the texture is seen obliquely
    constexpr float theta = 0.7f;
    constexpr float distance = 5.f;
    constexpr float scale = 2.6f;
    const float ct = std::cos(theta);
    const float st = std::sin(theta);

    // geometric frame of the quad (shared by all four corners)
    const std::array<float, 3> geo_normal{st, 0.f, ct};
    const std::array<float, 3> geo_tangent{ct, 0.f, -st};

    auto make_vertex = [&](float lx, float ly, float u, float v) {
        const float vx = ct * lx;
        const float vy = ly;
        const float vz = -st * lx - distance;
        const float w = -vz;
        const std::array<float, 4> clip{scale * vx, scale * vy, 0.3f * w, w};
        const std::array<float, 3> view_pos{vx, vy, vz};
        return cr::pack_lean_vertex(clip, view_pos, geo_normal, geo_tangent, 1.f, {u, v});
    };

    // corners: local (lx, ly) in [-1, 1], UVs in [0, 1], including the tile edges
    const auto a = make_vertex(-1.f, -1.f, 0.f, 0.f);
    const auto b = make_vertex(1.f, -1.f, 1.f, 0.f);
    const auto c = make_vertex(1.f, 1.f, 1.f, 1.f);
    const auto d = make_vertex(-1.f, 1.f, 0.f, 1.f);

    ctx.draw_triangle(a, b, c);
    ctx.draw_triangle(a, c, d);

    ctx.update_canvas();
    write_ppm(out_path, canvas.pixels, canvas.width, canvas.height);

    // brightness statistics confirm the specular normal pattern survived
    unsigned bright = 0;
    float maxv = 0.f;
    for (const cr::color& c : canvas.pixels)
    {
        maxv = std::max(maxv, std::max(c.r, std::max(c.g, c.b)));
        if (c.r > 0.25f || c.g > 0.25f || c.b > 0.25f) {
            ++bright;
        }
    }

    // a short console summary, useful when run from task_support/run.sh
    std::printf("LEAN_PPM %s %ux%u mip_levels=%u max_lod=%.3f lit_pixels=%u max_channel=%.3f\n",
                out_path, canvas.width, canvas.height, texture->level_count(),
                fs->max_lod, bright, maxv);
    return 0;
}
