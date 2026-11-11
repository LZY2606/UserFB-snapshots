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

// Same contract as rasterize(), but the primitive is first clipped against the
// view frustum (-w <= x,y,z <= w) in homogeneous coordinates, so vertices may
// lie outside the frustum or behind the camera. Pixels are sampled at their
// centers (x+.5, y+.5), only counter-clockwise front faces are kept, and edge
// ownership follows a half-open rule so adjacent triangles share boundary
// pixels exactly once. The fragment shader still receives perspective-correct
// barycentric weights expressed w.r.t. the three original vertices.
void rasterize_clipped(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

