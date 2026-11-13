#pragma once
#include <array>
#include <vector>
#include "tgaimage.h"
#include "geometry.h"
#include "our_gl.h"

// Output of a transparent fragment shader.
// rgb is non-premultiplized linear color, rgb and alpha are all in [0,1].
struct TFragment {
    bool   discard = false;
    double rgb[3]  = {0., 0., 0.};
    double alpha   = 0.;
};

struct ITransparencyShader {
    virtual TFragment fragment(const vec3 bar) const = 0;
    virtual ~ITransparencyShader() = default;
};

// Per-frame weighted-blended order-independent transparency accumulator
// (McGuire & Bavoil, 2013). It is created once per frame after the opaque
// pass, fed with transparent triangles in any order, and composited once
// onto the opaque framebuffer.
class TransparencyBuffer {
public:
    // The opaque depth buffer is borrowed, never modified: transparent
    // fragments only compare against it (equal depth counts as occluded).
    TransparencyBuffer(int width, int height, const std::vector<double> &opaque_depth);

    // Reset every accumulator for a new frame. The borrowed depth buffer
    // is re-read on every submit, so it must keep holding the frozen
    // opaque depth of the current frame.
    void clear();

    // Rasterize one transparent triangle through the same traversal as
    // rasterize() and fold every surviving fragment into the accumulators.
    void submit(const Triangle &clip, const ITransparencyShader &shader);

    // Composite the accumulated thin surfaces over the opaque background
    // stored in framebuffer. The buffer size must match the image; the
    // image must be RGB or RGBA with every alpha byte equal to 255.
    // Pixels without any contribution keep their background bytes.
    // Repeated calls are no-ops: compositing twice must not blend the
    // already composited image a second time.
    void composite(TGAImage &framebuffer);

    int width()  const { return w; }
    int height() const { return h; }

    // Debug readouts of the running floating-point accumulators.
    std::array<double,3> accumulated_rgb(int x, int y) const;
    double accumulated_alpha(int x, int y) const;
    double revealage(int x, int y) const;

private:
    int w = 0, h = 0;
    const std::vector<double> *depth = nullptr; // frozen opaque depth
    std::vector<std::array<double,3>> acc_rgb;  // A.rgb, zero-initialized
    std::vector<double> acc_alpha;              // A.a,   zero-initialized
    std::vector<double> reveal;                 // R,     initialized to 1
    bool composited = false;
};
