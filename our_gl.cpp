#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include "our_gl.h"
#include "shadow_volume.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<double> zbuffer;               // depth buffer

void lookat(const vec3 eye, const vec3 center, const vec3 up) {
    vec3 n = normalized(eye-center);
    vec3 l = normalized(cross(up,n));
    vec3 m = normalized(cross(n, l));
    ModelView = mat<4,4>{{{l.x,l.y,l.z,0}, {m.x,m.y,m.z,0}, {n.x,n.y,n.z,0}, {0,0,0,1}}} *
                mat<4,4>{{{1,0,0,-center.x}, {0,1,0,-center.y}, {0,0,1,-center.z}, {0,0,0,1}}};
}

void init_perspective(const double f) {
    Perspective = {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0, -1/f,1}}};
}

void init_viewport(const int x, const int y, const int w, const int h) {
    Viewport = {{{w/2., 0, 0, x+w/2.}, {0, h/2., 0, y+h/2.}, {0,0,1,0}, {0,0,0,1}}};
}

void init_zbuffer(const int width, const int height) {
    zbuffer = std::vector(width*height, -1000.);
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    vec4 ndc[3]    = { clip[0]/clip[0].w, clip[1]/clip[1].w, clip[2]/clip[2].w };                // normalized device coordinates
    vec2 screen[3] = { (Viewport*ndc[0]).xy(), (Viewport*ndc[1]).xy(), (Viewport*ndc[2]).xy() }; // screen coordinates

    mat<3,3> ABC = {{ {screen[0].x, screen[0].y, 1.}, {screen[1].x, screen[1].y, 1.}, {screen[2].x, screen[2].y, 1.} }};
    if (ABC.det()<1) return; // backface culling + discarding triangles that cover less than a pixel

    auto [bbminx,bbmaxx] = std::minmax({screen[0].x, screen[1].x, screen[2].x}); // bounding box for the triangle
    auto [bbminy,bbmaxy] = std::minmax({screen[0].y, screen[1].y, screen[2].y}); // defined by its top left and bottom right corners
#pragma omp parallel for
    for (int x=std::max<int>(bbminx, 0); x<=std::min<int>(bbmaxx, framebuffer.width()-1); x++) {         // clip the bounding box by the screen
        for (int y=std::max<int>(bbminy, 0); y<=std::min<int>(bbmaxy, framebuffer.height()-1); y++) {
            vec3 bc_screen = ABC.invert_transpose() * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
            vec3 bc_clip   = { bc_screen.x/clip[0].w, bc_screen.y/clip[1].w, bc_screen.z/clip[2].w };     // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
            bc_clip = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
            if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) continue; // negative barycentric coordinate => the pixel is outside the triangle
            double z = bc_screen * vec3{ ndc[0].z, ndc[1].z, ndc[2].z };   // linear interpolation of the depth
            if (z <= zbuffer[x+y*framebuffer.width()]) continue;   // discard fragments that are too deep w.r.t the z-buffer
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}

// ===========================================================================
// Shadow-volume path
// ===========================================================================

namespace {

struct ShadowFrame {
    int w = 0, h = 0;
    std::vector<double> depth;                 // visible-surface ndc depth z/w
    std::vector<unsigned char> valid;          // a visible opaque surface exists
    std::vector<vec3> albedo;                  // unquantized surface color
    std::vector<double> ambient;
    std::vector<double> direct;                // diffuse + specular
    std::vector<int> stencil;                  // signed z-fail count
};

struct ProjVert {
    vec2 s;            // screen position (y up), half-pixel centers are used
    double ndcz = 0;   // z/w, interpolated linearly in screen space
    double q[3] = {0,0,0}; // barycentric weight / w of the three original vertices
};

constexpr double FILL_EPS = 1e-10;

// Sutherland-Hodgman clipping in clip space. Planes are written so that the
// visible side satisfies dot(v, plane) >= 0.
struct ClipPlane { double a,b,c,d; };

double plane_side(const ClipPlane &p, const vec4 &v) {
    return p.a*v.x + p.b*v.y + p.c*v.z + p.d*v.w;
}

struct ClipNode {
    vec4 v;
    double b[3]; // clip-space barycentric weights of the original triangle
};

std::vector<ClipNode> clip_against(std::vector<ClipNode> poly, const ClipPlane &p) {
    std::vector<ClipNode> out;
    if (poly.empty()) return out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const ClipNode &cur  = poly[i];
        const ClipNode &prev = poly[(i + poly.size() - 1) % poly.size()];
        double ds = plane_side(p, cur.v);
        double dp = plane_side(p, prev.v);
        if (ds >= 0.0) {
            if (dp < 0.0) {
                double t = dp / (dp - ds);
                ClipNode n;
                n.v = prev.v + (cur.v - prev.v) * t;
                for (int k = 0; k < 3; ++k) n.b[k] = prev.b[k] + (cur.b[k] - prev.b[k]) * t;
                out.push_back(n);
            }
            out.push_back(cur);
        } else if (dp >= 0.0) {
            double t = dp / (dp - ds);
            ClipNode n;
            n.v = prev.v + (cur.v - prev.v) * t;
            for (int k = 0; k < 3; ++k) n.b[k] = prev.b[k] + (cur.b[k] - prev.b[k]) * t;
            out.push_back(n);
        }
    }
    return out;
}

std::vector<ProjVert> project_polygon(const std::vector<ClipNode> &poly,
                                      const mat<4,4> &viewport) {
    std::vector<ProjVert> out;
    out.reserve(poly.size());
    for (const ClipNode &n : poly) {
        vec4 ndc = n.v / n.v.w;
        vec2 scr = (viewport * ndc).xy();
        ProjVert pv;
        pv.s = scr;
        pv.ndcz = ndc.z;
        for (int k = 0; k < 3; ++k) pv.q[k] = n.b[k] / n.v.w;
        out.push_back(pv);
    }
    return out;
}

double signed_area(const ProjVert &a, const ProjVert &b, const ProjVert &c) {
    return (b.s.x-a.s.x)*(c.s.y-a.s.y) - (b.s.y-a.s.y)*(c.s.x-a.s.x);
}

// In y-up screen coordinates a top-left edge points downward, or is horizontal
// and points leftward. Such edges own samples lying exactly on them.
bool top_left(const vec2 &a, const vec2 &b) {
    double dx = b.x - a.x, dy = b.y - a.y;
    return dy < 0.0 || (dy == 0.0 && dx < 0.0);
}

// Half-pixel-center scan conversion of one positively oriented screen
// triangle. emit(pixel x, pixel y, screen-space barycentric weights, depth).
template <class Emit>
void scan_triangle(ProjVert a, ProjVert b, ProjVert c, int width, int height, Emit &&emit) {
    double area = signed_area(a, b, c);
    if (area == 0.0) return;            // degenerate projection covers no sample
    if (area < 0.0) { std::swap(b, c); area = -area; }

    // Edge i runs from vertex i to vertex (i+1); its edge function is the
    // barycentric weight of vertex (i+2).
    const ProjVert *v[3] = {&a, &b, &c};
    double ex[3] = {b.s.x-a.s.x, c.s.x-b.s.x, a.s.x-c.s.x};
    double ey[3] = {b.s.y-a.s.y, c.s.y-b.s.y, a.s.y-c.s.y};
    bool tl[3] = {top_left(a.s,b.s), top_left(b.s,c.s), top_left(c.s,a.s)};

    double bbminx = std::min({a.s.x, b.s.x, c.s.x});
    double bbmaxx = std::max({a.s.x, b.s.x, c.s.x});
    double bbminy = std::min({a.s.y, b.s.y, c.s.y});
    double bbmaxy = std::max({a.s.y, b.s.y, c.s.y});
    int x0 = std::max(0,        static_cast<int>(std::ceil(bbminx - 0.5)));
    int x1 = std::min(width-1,  static_cast<int>(std::floor(bbmaxx - 0.5)));
    int y0 = std::max(0,        static_cast<int>(std::ceil(bbminy - 0.5)));
    int y1 = std::min(height-1, static_cast<int>(std::floor(bbmaxy - 0.5)));

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            double sx = px + 0.5, sy = py + 0.5;
            double wgt[3];
            bool inside = true;
            for (int i = 0; i < 3; ++i) {
                double e = ex[i]*(sy - v[i]->s.y) - ey[i]*(sx - v[i]->s.x);
                if (e < 0.0) {
                    if (tl[i] && e >= -FILL_EPS) e = 0.0; // sample owned by a top-left edge
                    else { inside = false; break; }
                }
                wgt[(i+2)%3] = e;
            }
            if (!inside) continue;
            double inv = 1.0 / (wgt[0] + wgt[1] + wgt[2]);
            double bs[3] = {wgt[0]*inv, wgt[1]*inv, wgt[2]*inv};
            double z = bs[0]*a.ndcz + bs[1]*b.ndcz + bs[2]*c.ndcz;
            emit(px, py, bs, z, a, b, c);
        }
    }
}

// Clip an input triangle (clip coords + identity barycentrics) and turn it
// into a fan of projected screen triangles.
std::vector<ProjVert> clip_and_project(const vec4 clip[3], const mat<4,4> &viewport,
                                       bool far_clip, bool &front_facing) {
    static const ClipPlane planes_nofar[] = {
        {0,0,0, 1},  // w > 0    (eye plane), clipped first
        { 1,0,0, 1}, // x <= w   (right)
        {-1,0,0, 1}, // -x <= w  (left)
        { 0,1,0, 1}, // y <= w   (top)
        {0,-1,0, 1}, // -y <= w  (bottom)
        {0,0,-1, 1}, // z <= w   (near)
    };
    static const ClipPlane far_plane = {0,0,1,1}; // z >= -w (far)

    std::vector<ClipNode> poly(3);
    for (int i = 0; i < 3; ++i) {
        poly[i].v = clip[i];
        poly[i].b[0] = poly[i].b[1] = poly[i].b[2] = 0.0;
        poly[i].b[i] = 1.0;
    }
    for (const ClipPlane &p : planes_nofar) poly = clip_against(poly, p);
    if (far_clip) poly = clip_against(poly, far_plane);
    if (poly.size() < 3) return {};

    std::vector<ProjVert> pv = project_polygon(poly, viewport);
    // Clipping keeps the winding; use the polygon's signed (shoelace) area so
    // a degenerate first fan triangle cannot misclassify a non-degenerate fan.
    double poly_area = 0.0;
    for (size_t i = 0; i < pv.size(); ++i) {
        const ProjVert &a = pv[i];
        const ProjVert &b = pv[(i + 1) % pv.size()];
        poly_area += a.s.x * b.s.y - b.s.x * a.s.y;
    }
    if (poly_area == 0.0) return {};
    front_facing = poly_area > 0.0;
    return pv;
}

// Recover the perspective-correct barycentric weights of the three ORIGINAL
// vertices from the screen barycentrics of a (possibly clipped) fan triangle.
vec3 perspective_barycentrics(const ProjVert &a, const ProjVert &b,
                              const ProjVert &c, const double bs[3]) {
    double q[3] = {
        bs[0]*a.q[0] + bs[1]*b.q[0] + bs[2]*c.q[0],
        bs[0]*a.q[1] + bs[1]*b.q[1] + bs[2]*c.q[1],
        bs[0]*a.q[2] + bs[1]*b.q[2] + bs[2]*c.q[2],
    };
    double qsum = q[0] + q[1] + q[2];
    return {q[0]/qsum, q[1]/qsum, q[2]/qsum};
}

} // namespace

struct ShadowRenderer::Impl {
    ShadowFrame f;
    mat<4,4> viewport;
};

ShadowRenderer::ShadowRenderer(const int width, const int height, const mat<4,4> &viewport) {
    if (width <= 0 || height <= 0)
        throw std::runtime_error("ShadowRenderer: width and height must be positive");
    impl_ = new Impl();
    impl_->viewport = viewport;
    impl_->f.w = width;
    impl_->f.h = height;
    clear();
}

ShadowRenderer::~ShadowRenderer() { delete impl_; }

int ShadowRenderer::width()  const { return impl_->f.w; }
int ShadowRenderer::height() const { return impl_->f.h; }

void ShadowRenderer::clear() {
    ShadowFrame &f = impl_->f;
    size_t n = static_cast<size_t>(f.w) * f.h;
    f.depth.assign(n, -std::numeric_limits<double>::infinity());
    f.valid.assign(n, 0);
    f.albedo.assign(n, vec3{0,0,0});
    f.ambient.assign(n, 0.0);
    f.direct.assign(n, 0.0);
    f.stencil.assign(n, 0);
}

void ShadowRenderer::clear_stencil() {
    std::fill(impl_->f.stencil.begin(), impl_->f.stencil.end(), 0);
}

void ShadowRenderer::draw_surface(const int nfaces, ISurfaceShader &shader) {
    ShadowFrame &f = impl_->f;
    for (int face = 0; face < nfaces; ++face) {
        vec4 clip[3];
        SurfaceVaryings v[3] = {
            shader.vertex(face, 0, clip[0]),
            shader.vertex(face, 1, clip[1]),
            shader.vertex(face, 2, clip[2]),
        };
        bool front = true;
        std::vector<ProjVert> pv = clip_and_project(clip, impl_->viewport, true, front);
        if (pv.size() < 3) continue;
        for (size_t i = 1; i + 1 < pv.size(); ++i) {
            scan_triangle(pv[0], pv[i], pv[i+1], f.w, f.h,
                [&](int px, int py, const double bs[3], double z,
                    const ProjVert &A, const ProjVert &B, const ProjVert &C) {
                    int idx = px + py*f.w;
                    if (f.valid[idx] && z <= f.depth[idx]) return;
                    vec3 bar = perspective_barycentrics(A, B, C, bs);
                    SurfaceFragment frag = shader.fragment(v, bar);
                    if (frag.discard) return;       // discard writes neither depth nor material
                    f.depth[idx]   = z;
                    f.valid[idx]   = 1;
                    f.albedo[idx]  = frag.albedo;
                    f.ambient[idx] = frag.ambient;
                    f.direct[idx]  = frag.direct;
                });
        }
    }
}

void ShadowRenderer::draw_volume(const ShadowVolume &volume, const mat<4,4> &world2clip) {
    ShadowFrame &f = impl_->f;
    const auto &vv = volume.vertices();
    const auto &tt = volume.triangles();
    for (int tri = 0; tri < volume.ntris(); ++tri) {
        vec4 clip[3];
        for (int k = 0; k < 3; ++k) {
            vec3 p = vv[tt[3*tri + k]];
            clip[k] = world2clip * vec4{p.x, p.y, p.z, 1.0};
        }
        bool front = true;
        // Volumes are guaranteed not to be cut by the far plane; no far-plane
        // clipping and therefore no far-cap reconstruction is performed.
        std::vector<ProjVert> pv = clip_and_project(clip, impl_->viewport, false, front);
        if (pv.size() < 3) continue;
        for (size_t i = 1; i + 1 < pv.size(); ++i) {
            scan_triangle(pv[0], pv[i], pv[i+1], f.w, f.h,
                [&](int px, int py, const double /*bs*/[3], double z,
                    const ProjVert&, const ProjVert&, const ProjVert&) {
                    int idx = px + py*f.w;
                    if (!f.valid[idx]) return;            // background is never counted
                    if (z > f.depth[idx]) return;         // z-fail: only failing fragments count
                    // equal depth fails as well, so a lit near cap resting on
                    // the visible surface cancels its far cap (no self-shadow)
                    f.stencil[idx] += front ? -1 : +1;
                });
        }
    }
}

void ShadowRenderer::compose(TGAImage &framebuffer, const TGAColor &background) const {
    const ShadowFrame &f = impl_->f;
    if (framebuffer.width() != f.w || framebuffer.height() != f.h)
        throw std::runtime_error("ShadowRenderer::compose: framebuffer dimensions do not match the renderer");
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            int idx = x + y*f.w;
            if (!f.valid[idx]) { framebuffer.set(x, y, background); continue; }
            double visibility = f.stencil[idx] == 0 ? 1.0 : 0.0;
            double light = f.ambient[idx] + visibility * f.direct[idx];
            TGAColor out = background;
            for (int c = 0; c < 3; ++c) {
                double v = f.albedo[idx][c] * light;
                v = std::min(255.0, std::max(0.0, v));
                out[c] = static_cast<unsigned char>(v);
            }
            out[3] = 255;
            framebuffer.set(x, y, out);
        }
    }
}

const std::vector<double>&        ShadowRenderer::depth_buffer()  const { return impl_->f.depth; }
const std::vector<unsigned char>& ShadowRenderer::valid_buffer()  const { return impl_->f.valid; }
const std::vector<vec3>&          ShadowRenderer::albedo_buffer() const { return impl_->f.albedo; }
const std::vector<double>&        ShadowRenderer::ambient_buffer() const { return impl_->f.ambient; }
const std::vector<double>&        ShadowRenderer::direct_buffer() const { return impl_->f.direct; }
const std::vector<int>&           ShadowRenderer::stencil_buffer() const { return impl_->f.stencil; }
