#include <vector>
#include "tgaimage.h"
#include "geometry.h"

extern mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
extern std::vector<double> zbuffer;               // the depth buffer

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

struct IGBufferSink;
extern IGBufferSink *g_gbuffer_sink; // when set, rasterize records per-pixel view-space geometry

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
    // variant that also receives the pixel coordinates; the default keeps the old behavior
    virtual std::pair<bool,TGAColor> fragment_at(const vec3 bar, const int, const int) const { return fragment(bar); }
    // view-space geometry of the fragment, used for gbuffer capture; returning false records nothing
    virtual bool geometry(const vec3, vec3 &, vec3 &) const { return false; }
    virtual ~IShader() = default;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

