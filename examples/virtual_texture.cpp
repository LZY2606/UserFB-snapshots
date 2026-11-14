// Sparse virtual texture demo.
//
// Generates two small procedural textures (a logical mip chain each, stored
// page by page) and two full-screen quads whose depth ramps cross, so each
// layer occludes the other on one half of the image. A first frame is
// resolved with only the pinned 1x1 pages resident; its missing-page list is
// printed and written to disk, together with the fallback rendering. The
// requested pages are then uploaded and a second frame produces the
// high-resolution rendering.
//
// Usage: virtual_texture <output-directory>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include "tgaimage.h"
#include "virtual_texture.h"

using namespace vtex;

namespace {

constexpr int IMAGE = 64;     // viewport size
constexpr int BASE  = 16;     // base texture size
constexpr int PAGE  = 4;      // page edge

mat<4,4> demo_viewport() {
    return mat<4,4>{{{IMAGE/2., 0, 0, IMAGE/2.},
                     {0, IMAGE/2., 0, IMAGE/2.},
                     {0, 0, 1, 0},
                     {0, 0, 0, 1}}};
}

// One procedural texture: checkerboards for texture 0, concentric squares for
// texture 1. Colors are linear RGB doubles.
RGBd texel_color(int texture, int x, int y, int W, int H) {
    if (texture == 0) {
        int cell = ((x / 2) + (y / 2)) % 2;
        double fx = static_cast<double>(x) / W, fy = static_cast<double>(y) / H;
        return cell ? RGBd{0.85, 0.25 + 0.4 * fy, 0.15 + 0.5 * fx}
                    : RGBd{0.1 + 0.6 * fx, 0.2, 0.75 - 0.4 * fy};
    }
    double dx = static_cast<double>(x) / W - 0.5;
    double dy = static_cast<double>(y) / H - 0.5;
    double ring = std::fmod(std::floor((std::fabs(dx) + std::fabs(dy)) * 8.), 2.);
    return ring == 0. ? RGBd{0.1, 0.8, 0.6} : RGBd{0.95, 0.85, 0.25};
}

std::uint8_t to_byte(double v) {
    v = std::clamp(v, 0., 1.);
    return static_cast<std::uint8_t>(std::lround(v * 255.));
}

bool write_frame_tga(const VirtualTextureFrame &frame, const std::string &path) {
    TGAImage img(frame.width(), frame.height(), TGAImage::RGB, TGAColor{{0, 0, 0, 255}, 4});
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x) {
            const FragmentResult &f = frame.fragment(x, y);
            TGAColor c = TGAColor{{to_byte(f.color.b), to_byte(f.color.g), to_byte(f.color.r), 255}, 4}; // BGRA
            img.set(x, y, c);
        }
    return img.write_tga_file(path);
}

// Two full-screen quads whose depth ramps cross at the screen diagonal: layer
// 0 is near on the lower-right half, layer 1 on the upper-left half, so each
// layer occludes the other on one half of the image.
void submit_scene(VirtualTextureFrame &frame) {
    vec2 ua[3] = { vec2{0, 0}, vec2{1, 0}, vec2{0, 1} };
    vec2 ub[3] = { vec2{1, 0}, vec2{1, 1}, vec2{0, 1} };
    vec4 a0[3] = { vec4{-1, -1, -0.6, 1}, vec4{1, -1, 0.9, 1}, vec4{-1, 1, -0.6, 1} };
    vec4 a1[3] = { vec4{ 1, -1, 0.9, 1}, vec4{1,  1, 0.9, 1}, vec4{-1, 1, -0.6, 1} };
    vec4 b0[3] = { vec4{-1, -1, 0.9, 1},  vec4{1, -1, -0.6, 1}, vec4{-1, 1, 0.9, 1} };
    vec4 b1[3] = { vec4{ 1, -1, -0.6, 1}, vec4{1,  1, -0.6, 1}, vec4{-1, 1, 0.9, 1} };
    frame.submit(a0, ua, 0);
    frame.submit(a1, ub, 0);
    frame.submit(b0, ua, 1);
    frame.submit(b1, ub, 1);
}

void write_feedback(const std::vector<PageKey> &keys, const std::string &path) {
    std::ofstream out(path);
    out << "# texture level page_y page_x\n";
    for (const PageKey &k : keys)
        out << k.texture << ' ' << k.level << ' ' << k.page_y << ' ' << k.page_x << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const std::string dir = argv[1];
    try {
        SparseTexturePool pool;
        pool.declare_texture(0, TextureDesc{BASE, BASE, PAGE});
        pool.declare_texture(1, TextureDesc{BASE, BASE, PAGE});

        // Only the pinned 1x1 level of each texture is resident initially.
        for (int tex = 0; tex < 2; ++tex) {
            int last = pool.level_count(tex) - 1;
            pool.upload_page(tex, last, 0, 0,
                             PageData{1, 1, {texel_color(tex, 0, 0, 1, 1)}});
        }

        mat<4,4> vp = demo_viewport();

        VirtualTextureFrame before(IMAGE, IMAGE, pool.snapshot(), vp);
        submit_scene(before);
        before.resolve();

        std::cout << "missing pages before upload (" << before.missing_pages().size() << "):\n";
        for (const PageKey &k : before.missing_pages())
            std::cout << "  texture=" << k.texture << " level=" << k.level
                      << " page_y=" << k.page_y << " page_x=" << k.page_x << '\n';
        write_feedback(before.missing_pages(), dir + "/missing_pages.txt");
        if (!write_frame_tga(before, dir + "/virtual_texture_before.tga"))
            throw std::runtime_error("failed to write before TGA");

        // Satisfy exactly the reported requests, level 0 pages only.
        for (const PageKey &k : before.missing_pages()) {
            int ew, eh;
            pool.page_extent(k.texture, k.level, k.page_x, k.page_y, ew, eh);
            PageData page;
            page.width = ew;
            page.height = eh;
            page.pixels.resize(static_cast<size_t>(ew) * eh);
            int W, H;
            pool.level_size(k.texture, k.level, W, H);
            for (int ly = 0; ly < eh; ++ly)
                for (int lx = 0; lx < ew; ++lx)
                    page.pixels[lx + ly * ew] =
                        texel_color(k.texture, k.page_x * PAGE + lx, k.page_y * PAGE + ly, W, H);
            pool.upload_page(k.texture, k.level, k.page_x, k.page_y, page);
        }

        VirtualTextureFrame after(IMAGE, IMAGE, pool.snapshot(), vp);
        submit_scene(after);
        after.resolve();
        std::cout << "missing pages after upload: " << after.missing_pages().size() << '\n';
        if (!write_frame_tga(after, dir + "/virtual_texture_after.tga"))
            throw std::runtime_error("failed to write after TGA");

        // Sanity: the two renderings actually differ, and both layers win some
        // pixels (mutual occlusion).
        bool differs = false;
        bool face0 = false, face1 = false;
        for (int y = 0; y < IMAGE; ++y)
            for (int x = 0; x < IMAGE; ++x) {
                const FragmentResult &p = before.fragment(x, y), &q = after.fragment(x, y);
                if (!(p.color.r == q.color.r && p.color.g == q.color.g && p.color.b == q.color.b))
                    differs = true;
                if (q.face == 0 || q.face == 1) face0 = true;
                if (q.face == 2 || q.face == 3) face1 = true;
            }
        if (!differs || !face0 || !face1)
            throw std::runtime_error("demo self-check failed");
        std::cout << "wrote " << dir << "/virtual_texture_before.tga, "
                  << dir << "/virtual_texture_after.tga and "
                  << dir << "/missing_pages.txt\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "virtual_texture demo failed: " << e.what() << '\n';
        return 1;
    }
}
