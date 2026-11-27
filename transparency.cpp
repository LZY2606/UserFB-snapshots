#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "transparency.h"

TransparencyBuffer::TransparencyBuffer(int width, int height, double zbuffer_clear)
    : w(width), h(height), accum(static_cast<size_t>(width)*height),
      opaque_depth(static_cast<size_t>(width)*height, zbuffer_clear) {
    if (width<=0 || height<=0) throw std::invalid_argument("TransparencyBuffer: bad size");
}

void TransparencyBuffer::clear() {
    std::fill(accum.begin(), accum.end(), Pixel{});
    // Drop the captured composite background; the depth source binding stays so
    // that a fresh submit() can snapshot the new frame's opaque depth.
    have_frozen_background = false;
    frozen_background.clear();
    frozen_bpp = 0;
}

void TransparencyBuffer::set_opaque_depth(const std::vector<double> &depth) {
    if (depth.size() != accum.size())
        throw std::invalid_argument("TransparencyBuffer: opaque depth size mismatch");
    opaque_depth = depth;
    depth_source = &depth;
}

void TransparencyBuffer::submit() {
    // Refresh the frozen snapshot from the depth buffer installed by
    // set_opaque_depth(). The transparency path itself never writes it.
    if (depth_source) {
        if (depth_source->size() != accum.size())
            throw std::invalid_argument("TransparencyBuffer: opaque depth size mismatch");
        opaque_depth = *depth_source;
    }
}

void TransparencyBuffer::triangle(const Triangle &clip, const ITransparentShader &shader) {
    RasterTriangle tri = prepare_raster_triangle(clip, w, h);
    if (!tri.valid) return;
#pragma omp parallel for
    for (int x=tri.bbminx; x<=tri.bbmaxx; x++) {
        for (int y=tri.bbminy; y<=tri.bbmaxy; y++) {
            vec3 bc_clip;
            double depth;
            if (!raster_coverage(tri, clip, x, y, bc_clip, depth)) continue;
            const size_t idx = static_cast<size_t>(x)+static_cast<size_t>(y)*w;
            if (depth <= opaque_depth[idx]) continue; // opaque geometry, equality included
            TransparentFragment f = shader.fragment(bc_clip);
            if (f.discard || f.alpha<=0.) continue;
            const double alpha = std::clamp(f.alpha, 0., 1.);
            if (alpha<=0.) continue;
            const double weight = std::clamp(0.01 + 8.*alpha, 0.01, 8.)
                                * std::clamp(1. + depth, 0.1, 2.);
            const vec3 rgb{std::clamp(f.rgb.x, 0., 1.),
                           std::clamp(f.rgb.y, 0., 1.),
                           std::clamp(f.rgb.z, 0., 1.)};
            Pixel &p = accum[idx];
            p.r += rgb.x * alpha * weight;
            p.g += rgb.y * alpha * weight;
            p.b += rgb.z * alpha * weight;
            p.a += alpha * weight;
            p.T *= 1. - alpha;
        }
    }
}

vec3 TransparencyBuffer::accumulated_rgb(int x, int y) const {
    if (!in_bounds(x, y)) return {0,0,0};
    const Pixel &p = pixel(x, y);
    return {p.r, p.g, p.b};
}

double TransparencyBuffer::accumulated_alpha(int x, int y) const {
    if (!in_bounds(x, y)) return 0;
    return pixel(x, y).a;
}

double TransparencyBuffer::transmittance(int x, int y) const {
    if (!in_bounds(x, y)) return 1;
    return pixel(x, y).T;
}

void TransparencyBuffer::composite(TGAImage &framebuffer) const {
    if (framebuffer.width()!=w || framebuffer.height()!=h)
        throw std::invalid_argument("TransparencyBuffer: framebuffer size mismatch");
    const TGAColor probe = framebuffer.get(0, 0);
    const int bpp = probe.bytespp;
    if (bpp!=3 && bpp!=4)
        throw std::invalid_argument("TransparencyBuffer: background must be RGB or RGBA");

    // Freeze the opaque background once per frame, so that calling composite()
    // again cannot blend the previous result back in.
    if (!have_frozen_background) {
        frozen_bpp = bpp;
        frozen_background.resize(static_cast<size_t>(w)*h*bpp);
        for (int y=0; y<h; y++)
            for (int x=0; x<w; x++) {
                TGAColor c = framebuffer.get(x, y);
                if (bpp==4 && c[3]!=255)
                    throw std::invalid_argument("TransparencyBuffer: RGBA background alpha must be 255");
                for (int i=0; i<bpp; i++)
                    frozen_background[(static_cast<size_t>(x)+static_cast<size_t>(y)*w)*bpp+i] = c[i];
            }
        have_frozen_background = true;
    }
    if (frozen_bpp!=bpp)
        throw std::invalid_argument("TransparencyBuffer: framebuffer format changed between composites");

    for (int y=0; y<h; y++)
        for (int x=0; x<w; x++) {
            const size_t idx = static_cast<size_t>(x)+static_cast<size_t>(y)*w;
            const Pixel &p = accum[idx];
            const double coverage = 1. - p.T;
            if (p.a<=0. || coverage<=0.) continue; // no contribution: keep background bytes

            const vec3 average{p.r/p.a, p.g/p.a, p.b/p.a};
            TGAColor out;
            out.bytespp = static_cast<std::uint8_t>(bpp);
            const std::uint8_t *bg = &frozen_background[idx*bpp];
            for (int i=0; i<3; i++) {
                // TGAColor is BGRA: channel 0 is blue, so RGB maps in reverse.
                const double bg_linear = bg[i]/255.;
                const double mixed = bg_linear*(1.-coverage) + average[2-i]*coverage;
                const long rounded = std::lround(std::clamp(mixed, 0., 1.)*255.);
                out[i] = static_cast<std::uint8_t>(rounded);
            }
            out[3] = 255; // no alpha in an RGB frame, but keep the logical byte at 255
            framebuffer.set(x, y, out);
        }
}
