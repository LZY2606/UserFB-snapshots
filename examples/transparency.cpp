#include <iostream>
#include <stdexcept>
#include "our_gl.h"
#include "transparency.h"

namespace {
constexpr int W = 48;
constexpr int H = 32;

struct SolidOpaque : IShader {
    TGAColor color;
    explicit SolidOpaque(TGAColor c) : color(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, color}; }
};

struct ThinSurface : ITransparencyShader {
    double rgb[3];
    double alpha;
    ThinSurface(double r, double g, double b, double a) : rgb{r, g, b}, alpha(a) {}
    TFragment fragment(vec3) const override { return {false, {rgb[0], rgb[1], rgb[2]}, alpha}; }
};

// Quad in normalized device coordinates (w = 1) split into two triangles.
// Depth is assigned per corner and interpolated by the rasterizer, so two
// overlapping quads can genuinely intersect instead of merely overlapping.
void submit_quad(TransparencyBuffer &tb, const ITransparencyShader &sh,
                 double x0, double y0, double x1, double y1,
                 double (*depth_at)(double, double)) {
    const vec4 p00{x0, y0, depth_at(x0, y0), 1.};
    const vec4 p11{x1, y1, depth_at(x1, y1), 1.};
    const vec4 p10{x1, y0, depth_at(x1, y0), 1.};
    const vec4 p01{x0, y1, depth_at(x0, y1), 1.};
    Triangle t0 = {p00, p10, p11};
    Triangle t1 = {p00, p11, p01};
    tb.submit(t0, sh);
    tb.submit(t1, sh);
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " output.tga\n";
        return 1;
    }

    init_viewport(0, 0, W, H);
    init_zbuffer(W, H);
    TGAImage framebuffer(W, H, TGAImage::RGB);

    // Checkerboard background, painted straight into the opaque framebuffer.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const bool dark = ((x / 6) + (y / 6)) % 2 == 0;
            const std::uint8_t v = dark ? 60 : 220;
            framebuffer.set(x, y, TGAColor{v, v, v, 255});
        }
    }

    // Opaque occluder at depth 0; transparent fragments at depth <= 0 are
    // rejected by the frozen depth buffer, equal depth included.
    SolidOpaque occluder(TGAColor{40, 120, 40, 255}); // greenish, BGRA
    Triangle block = { vec4{-1., -1., 0., 1.},
                       vec4{-0.1, -1., 0., 1.},
                       vec4{-0.45, 0.4, 0., 1.} };
    rasterize(block, occluder, framebuffer);

    TransparencyBuffer tb(W, H, zbuffer);

    // Red vertical sheet and blue horizontal sheet, their depth slopes
    // cross inside the overlap region (line y = -x - 0.1).
    auto red_depth  = [](double x, double) { return 0.2 + 0.5*x; };
    auto blue_depth = [](double, double y) { return 0.15 - 0.5*y; };
    ThinSurface red(1., 0., 0., 0.55);
    ThinSurface blue(0., 0., 1., 0.45);

    // Submission order is arbitrary: weighted blending is order independent.
    submit_quad(tb, blue, -1., -0.7, 1., 0.3, blue_depth);
    submit_quad(tb, red,  -0.7, -1., 0.3, 1., red_depth);

    tb.composite(framebuffer);
    if (!framebuffer.write_tga_file(argv[1]))
        throw std::runtime_error("failed to write the output TGA");
    std::cout << "wrote " << W << "x" << H << " transparent demo to " << argv[1] << "\n";
    return 0;
}
