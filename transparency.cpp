#include <cmath>
#include <cstdint>
#include <stdexcept>
#include "transparency.h"

namespace {
double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}
}

TransparencyBuffer::TransparencyBuffer(int width, int height, const std::vector<double> &opaque_depth)
    : w(width), h(height), depth(&opaque_depth),
      acc_rgb(width*height, {0., 0., 0.}),
      acc_alpha(width*height, 0.),
      reveal(width*height, 1.) {
    if (width<=0 || height<=0)
        throw std::runtime_error("transparency buffer needs a positive size");
    if (static_cast<int>(opaque_depth.size()) != width*height)
        throw std::runtime_error("transparency buffer size must match the color and depth buffers");
}

void TransparencyBuffer::clear() {
    std::fill(acc_rgb.begin(), acc_rgb.end(), std::array<double,3>{0., 0., 0.});
    std::fill(acc_alpha.begin(), acc_alpha.end(), 0.);
    std::fill(reveal.begin(), reveal.end(), 1.);
    composited = false;
}

void TransparencyBuffer::submit(const Triangle &clip, const ITransparencyShader &shader) {
    raster_traverse(clip, w, h, [&](const FragmentSample &f) {
        const int idx = f.x + f.y*w;
        // Transparent fragments only fight the frozen opaque depth.
        // Equal depth counts as occluded, and earlier transparent surfaces
        // never write depth, so they cannot hide later ones.
        if (f.z <= (*depth)[idx]) return;

        TFragment out = shader.fragment(f.bar);
        if (out.discard || out.alpha <= 0.) return; // discard and zero alpha leave nothing behind

        const double alpha = out.alpha;
        const double weight = clampd(0.01 + 8.*alpha, 0.01, 8.)
                            * clampd(1. + f.z,       0.1,  2.);
        const double wa = alpha * weight;
        for (int c = 0; c < 3; c++)
            acc_rgb[idx][c] += out.rgb[c] * wa;
        acc_alpha[idx] += wa;
        reveal[idx]    *= 1. - alpha;
    });
}

void TransparencyBuffer::composite(TGAImage &framebuffer) {
    if (framebuffer.width()!=w || framebuffer.height()!=h)
        throw std::runtime_error("framebuffer size does not match the transparency buffer");
    const int bpp = [&] {
        TGAColor probe = framebuffer.get(0, 0);
        return probe.bytespp;
    }();
    if (bpp != TGAImage::RGB && bpp != TGAImage::RGBA)
        throw std::runtime_error("compositing requires an RGB or RGBA framebuffer");
    if (bpp == TGAImage::RGBA) { // the whole RGBA background must be fully opaque
        for (int i = 0; i < w*h; i++) {
            TGAColor px = framebuffer.get(i % w, i / w);
            if (px[3] != 255)
                throw std::runtime_error("RGBA background must be fully opaque (alpha 255)");
        }
    }
    if (composited) return; // compositing twice must not re-blend the previous result

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int idx = x + y*w;
            if (acc_alpha[idx] == 0.) continue; // no contribution: background bytes stay untouched

            TGAColor bg = framebuffer.get(x, y);

            const double coverage = 1. - reveal[idx];           // 1-R
            const double         tint[3] = { acc_rgb[idx][0]/acc_alpha[idx],
                                             acc_rgb[idx][1]/acc_alpha[idx],
                                             acc_rgb[idx][2]/acc_alpha[idx] }; // A.rgb/A.a
            // TGA stores BGRA; tint and the background are linear normalized values.
            const int channel[3] = {2, 1, 0}; // R, G, B slots in the bgra byte array
            for (int c = 0; c < 3; c++) {
                const double background = bg[channel[c]] / 255.;
                const double blended    = tint[c]*coverage + background*(1.-coverage);
                const long   byte_v     = std::lround(clampd(blended, 0., 1.) * 255.);
                bg[channel[c]] = static_cast<std::uint8_t>(byte_v);
            }
            if (bpp == TGAImage::RGBA) bg[3] = 255;
            framebuffer.set(x, y, bg);
        }
    }
    composited = true;
}

std::array<double,3> TransparencyBuffer::accumulated_rgb(int x, int y) const {
    return acc_rgb.at(x + y*w);
}

double TransparencyBuffer::accumulated_alpha(int x, int y) const {
    return acc_alpha.at(x + y*w);
}

double TransparencyBuffer::revealage(int x, int y) const {
    return reveal.at(x + y*w);
}
