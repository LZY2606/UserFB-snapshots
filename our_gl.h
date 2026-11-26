#include "tgaimage.h"
#include "geometry.h"

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

// --- Shared rasterization helpers, used by both the legacy pipeline above
//     and the sparse virtual texture path. They never touch global state. ---

// Project the three clip-space vertices with the given viewport matrix.
void gl_project_triangle(const vec4 clip[3], const mat<4,4> &viewport,
                         vec4 ndc[3], vec2 screen[3]);

// Bounding box of the projected triangle, clipped to the [0,w-1]x[0,h-1] image.
struct PixelBounds { int x0, y0, x1, y1; };
PixelBounds gl_pixel_bounds(const vec2 screen[3], int width, int height);

// Screen-space barycentric coordinates of pixel (x,y); the same matrix the
// legacy rasterizer builds. A triangle covering less than a pixel (or wound
// backwards) is rejected up front, matching the legacy area culling.
bool gl_screen_barycentric(const vec2 screen[3], int x, int y, vec3 &bc_screen);

// Perspective-correct barycentric coordinates in clip space, obtained from the
// screen-space ones and the clip w components.
vec3 gl_perspective_barycentric(const vec3 &bc_screen, const vec4 clip[3]);

// Interpolated NDC depth; larger values are closer, as in the legacy pipeline.
double gl_interpolated_depth(const vec3 &bc_screen, const vec4 ndc[3]);
