#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "geometry.h"
#include "our_gl.h"
#include "tgaimage.h"

// Fragment emitted by a transparency shader. rgb is non-premultiplied linear
// RGB in [0,1], alpha is in [0,1], discard removes the fragment entirely.
struct TransparentFragment {
    vec3 rgb{0,0,0};
    double alpha = 0;
    bool discard = false;
};

// Shader for the transparent path. Unlike IShader, colors are floating point
// linear values; the perspective-correct barycentric coordinates are the same
// ones the opaque path passes to IShader::fragment.
struct ITransparentShader {
    virtual TransparentFragment fragment(const vec3 bar) const = 0;
    virtual ~ITransparentShader() = default;
};

// Weighted-blended transparency buffer, created per frame. It never reads or
// writes the opaque depth buffer; fragments are compared only against the
// opaque depth captured by submit() at submission time.
//
// Accumulation per pixel:  A.rgb += RGB*alpha*w
//                          A.a   += alpha*w
//                          R     *= 1-alpha
// with A.rgb = A.a = 0, R = 1 initially and
// w = clamp(0.01+8*alpha, 0.01, 8) * clamp(1+z/w, 0.1, 2), where z/w is the
// fragment's ndc depth. Transparent surfaces are never depth-sorted: any
// submission order gives the same (order-independent) weighted average.
class TransparencyBuffer {
public:
    // The buffer size must match the color and depth buffers of the frame.
    TransparencyBuffer(int width, int height, double zbuffer_clear = -1000.);

    // Clear the accumulator and restore R=1, ready for the next frame.
    void clear();

    // Borrow the frozen opaque depth buffer for this frame. Until submit() is
    // called the opaque path may keep writing zbuffer; submit() snapshots it.
    void set_opaque_depth(const std::vector<double> &depth);
    // Freeze the currently installed opaque depth buffer. The transparent path
    // only ever compares against this snapshot, and never modifies it.
    void submit();

    // Submit a triangle in clip coordinates. Uses the same NDC/viewport,
    // backface culling, coverage and "larger z/w is nearer" conventions as
    // rasterize(), with depth equality counting as occluded. Does not update
    // the opaque depth buffer.
    void triangle(const Triangle &clip, const ITransparentShader &shader);

    // Accumulated premultiplied RGB (A.rgb) and alpha sum (A.a) at a pixel.
    vec3 accumulated_rgb(int x, int y) const;
    double accumulated_alpha(int x, int y) const;
    // Accumulated transmittance R at a pixel (1 where nothing was drawn).
    double transmittance(int x, int y) const;

    // Composite over an opaque RGB or RGBA (alpha=255) image: the average
    // color A.rgb/A.a is blended with the frozen background using coverage
    // 1-R. Pixels with no contribution keep their background bytes untouched.
    // Calling this twice produces the same image.
    void composite(TGAImage &framebuffer) const;

    int width()  const { return w; }
    int height() const { return h; }

private:
    struct Pixel {
        double r = 0, g = 0, b = 0; // A.rgb
        double a = 0;               // A.a
        double T = 1;               // R
    };

    bool in_bounds(int x, int y) const { return x>=0 && y>=0 && x<w && y<h; }
    const Pixel &pixel(int x, int y) const { return accum[x+y*w]; }

    int w = 0, h = 0;
    std::vector<Pixel> accum;
    std::vector<double> opaque_depth; // frozen copy of the opaque z-buffer
    const std::vector<double> *depth_source = nullptr; // live depth buffer installed before submit()
    mutable std::vector<std::uint8_t> frozen_background; // opaque background captured by first composite()
    mutable int frozen_bpp = 0;
    mutable bool have_frozen_background = false;
};
