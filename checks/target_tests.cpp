// Regression / conformance tests for perspective-correct texture mapping.
//
// Every test drives the public TinyGL entry points (glVertex/glTexCoord/...),
// runs the real rasterizer, and compares the framebuffer against an
// independent double precision reference. The reference uses only the vertex
// data and documented conventions:
//
//   * integer screen coordinates: sx = (int)(clip.x/w * scaleX + transX)
//   * screen barycentric weights of the integer vertex positions
//   * rational texture interpolation
//         u = (sum l_i * u_i / w_i) / (sum l_i / w_i)
//     followed by the engine's fixed point nearest/repeat sampling
//   * the engine's fixed point colour modulation
//
// The RGB texture is generated procedurally: texel (x,y) = (x, y, 3), so the
// sampled colour identifies the source texel (the red/green bytes are the
// texel coordinates modulo the modulation factor).
//
// Identity projection note: the library has an existing fast path that
// assumes model W = 1 when the projection matrix does not transform W. Tests
// that need real per-vertex w therefore install a frustum (or a custom)
// projection matrix; the orthographic tests deliberately keep the identity
// projection.

#include "suite.h"

extern "C" {
#include "zbuffer.h"
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int W = 32;
const int H = 32;
const int TEX_DIM = TGL_FEATURE_TEXTURE_DIM;

// Fixed point texture addressing constants (public header).
const long S_MIN = ZB_POINT_S_MIN;
const long S_RANGE = (long)(ZB_POINT_S_MAX - ZB_POINT_S_MIN);
const long T_MIN = ZB_POINT_T_MIN;
const long T_RANGE = (long)(ZB_POINT_T_MAX - ZB_POINT_T_MIN);

// The colour glColor3f(1,1,1) is stored as this fixed point value by
// glColor4f/gl_transform_to_viewport.
const unsigned WHITE_FIXED = (COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT) & COLOR_MASK;

struct Pixel {
    unsigned r, g, b;
    bool operator==(const Pixel& o) const { return r == o.r && g == o.g && b == o.b; }
    bool near(const Pixel& o, int tol) const {
        return std::abs((long)r - (long)o.r) <= tol &&
               std::abs((long)g - (long)o.g) <= tol &&
               std::abs((long)b - (long)o.b) <= tol;
    }
    std::string str() const {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%02x%02x%02x", r, g, b);
        return buf;
    }
};

static Pixel unpack(unsigned p) { return Pixel{(p >> 16) & 0xffu, (p >> 8) & 0xffu, p & 0xffu}; }

// Emulate RGB_MIX_FUNC with a fixed point colour value.
//   RGB_TO_PIXEL((c*GET_RED(t))>>8, ...) keeps bits 16..23 of the product.
static Pixel modulate_fixed(unsigned cr, unsigned cg, unsigned cb, Pixel t) {
    unsigned tpix = (t.r << 16) | (t.g << 8) | t.b;
    unsigned r = (cr * (tpix >> 16)) >> 8;
    unsigned g = (cg * ((tpix >> 8) & 0xff)) >> 8;
    unsigned b = (cb * (tpix & 0xff)) >> 8;
    return Pixel{(r >> 16) & 0xff, (g >> 16) & 0xff, (b >> 16) & 0xff};
}

static Pixel modulate_white(Pixel t) { return modulate_fixed(WHITE_FIXED, WHITE_FIXED, WHITE_FIXED, t); }

// Colour fixed point conversion performed by glColor4f + the vertex
// transform (for an untextured flat/smooth draw).
static unsigned color_fixed(double c) {
    return (unsigned)((GLint)(c * (double)COLOR_CORRECTED_MULT_MASK + (double)COLOR_MIN_MULT)) & COLOR_MASK;
}
// RGB_TO_PIXEL applied to a fixed point colour triple:
//   COLOR_R_GET32(r)=r&0xff0000, COLOR_G_GET32(g)=(g>>8)&0xff00,
//   COLOR_B_GET32(b)=(b>>16)&0xff, then unpacked to r,g,b bytes.
static Pixel color_pixel(unsigned r, unsigned g, unsigned b) {
    return Pixel{(r & 0xff0000u) >> 16, ((g >> 8) & 0xff00u) >> 8, (b >> 16) & 0xffu};
}

// ---------------------------------------------------------------------------
struct TestTexture {
    std::vector<unsigned char> rgb;  // TEX_DIM*TEX_DIM*3, texel (x,y) = (x,y,3)
    GLuint id = 0;
    TestTexture() : rgb((size_t)TEX_DIM * TEX_DIM * 3) { init_procedural(); }
    void init_procedural() {
        for (int y = 0; y < TEX_DIM; y++)
            for (int x = 0; x < TEX_DIM; x++) {
                unsigned char* p = &rgb[((size_t)y * TEX_DIM + x) * 3];
                p[0] = (unsigned char)x;
                p[1] = (unsigned char)y;
                p[2] = 3;
            }
    }
    void upload() {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, 3, TEX_DIM, TEX_DIM, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    }
    Pixel texel(int tx, int ty) const {
        tx &= (TEX_DIM - 1);  // repeat
        ty &= (TEX_DIM - 1);
        const unsigned char* p = &rgb[((size_t)ty * TEX_DIM + tx) * 3];
        return Pixel{p[0], p[1], p[2]};
    }
};

// ---------------------------------------------------------------------------
struct Viewport {
    double sx, tx, sy, ty, sz, tz;
    Viewport(int w, int h) {
        sx = (w - 0.5) / 2.0;
        tx = ((w - 0.5) / 2.0);
        sy = -(h - 0.5) / 2.0;
        ty = ((h - 0.5) / 2.0);
        double zsize = (double)(1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
        sz = -((zsize - 0.5) / 2.0);
        tz = ((zsize - 0.5) / 2.0) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2.0;
    }
};

// Post-projection integer vertex, matching the library's GLfloat arithmetic.
struct RasterPoint {
    int x, y, z;
    float w;
};

static RasterPoint project(const Viewport& vp, double xc, double yc, double zc, double wc) {
    RasterPoint p;
    float x = (float)xc, y = (float)yc, z = (float)zc, w = (float)wc;
    float inv = 1.0f / w;
    float sx = (float)vp.sx, tx = (float)vp.tx;
    float sy = (float)vp.sy, ty = (float)vp.ty;
    float sz = (float)vp.sz, tz = (float)vp.tz;
    p.x = (int)(x * inv * sx + tx);
    p.y = (int)(y * inv * sy + ty);
    p.z = (int)(z * inv * sz + tz);
    p.w = w;
    return p;
}

static bool barycentric(const RasterPoint& a, const RasterPoint& b, const RasterPoint& c,
                        double px, double py, double& l0, double& l1, double& l2) {
    double det = ((double)b.x - a.x) * ((double)c.y - a.y) -
                 ((double)c.x - a.x) * ((double)b.y - a.y);
    if (det == 0.0) return false;
    l1 = (((double)px - a.x) * ((double)c.y - a.y) -
          ((double)c.x - a.x) * ((double)py - a.y)) / det;
    l2 = (((double)b.x - a.x) * ((double)py - a.y) -
          ((double)px - a.x) * ((double)b.y - a.y)) / det;
    l0 = 1.0 - l1 - l2;
    return true;
}

// Fixed point endpoint conversion exactly as in vertex.c/clip.c:
//   (GLint)(coord * (MAX-MIN) + MIN)   (GLfloat arithmetic).
static long to_fixed_s(double u) {
    float f = (float)u * (float)S_RANGE + (float)S_MIN;
    return (long)(GLint)f;
}
static long to_fixed_t(double v) {
    float f = (float)v * (float)T_RANGE + (float)T_MIN;
    return (long)(GLint)f;
}

// Nearest/repeat sample from fixed point s/t following
// ST_TO_TEXTURE_BYTE_OFFSET; the byte offset / PSZB is the row-major pixel
// index ty*TEX_DIM+tx.
static Pixel sample_fixed(const TestTexture& tex, long s, long t) {
    long byte_ofs = ((s & ZB_S_MASK) >> (ZB_POINT_S_VALUE - PSZSH)) |
                    ((t & ZB_T_MASK) >> (ZB_POINT_T_VALUE - PSZSH));
    long pix_index = byte_ofs / PSZB;
    int tx = (int)(pix_index % TEX_DIM);
    int ty = (int)(pix_index / TEX_DIM);
    return tex.texel(tx, ty);
}

// True when the fixed point coordinate sits within `guard` units of a nearest
// boundary or the repeat wrap, where an implementation difference of one
// ulp could legitimately pick the neighbouring texel.
static bool near_texel_boundary(long s, long t, long guard = 8) {
    // A texel boundary lies at every multiple of 2^(FRAC_BITS+1) of the fixed
    // point value (the masks use the absolute value, not an offset).
    const long step = 1L << (ZB_POINT_S_FRAC_BITS + 1);
    const long tstep = 1L << (ZB_POINT_T_FRAC_BITS + 1);
    long fs = s % step;
    long ft = t % tstep;
    if (fs < 0) fs += step;
    if (ft < 0) ft += tstep;
    return fs < guard || fs > step - guard || ft < guard || ft > tstep - guard;
}

struct RefPixel {
    Pixel color;
    bool near_boundary = false;
};

// Per-pixel rational reference. `uv` are the normalised texture coordinates
// at the three vertices, `a/b/c` their integer screen positions and w.
static RefPixel reference_pixel(const TestTexture& tex, const RasterPoint& a, const RasterPoint& b,
                                const RasterPoint& c, const double (*uv)[2], double px, double py) {
    RefPixel out{Pixel{0, 0, 0}, false};
    double l0, l1, l2;
    if (!barycentric(a, b, c, px, py, l0, l1, l2)) return out;
    double iw = l0 / a.w + l1 / b.w + l2 / c.w;
    long s0 = to_fixed_s(uv[0][0]), s1 = to_fixed_s(uv[1][0]), s2 = to_fixed_s(uv[2][0]);
    long t0 = to_fixed_t(uv[0][1]), t1 = to_fixed_t(uv[1][1]), t2 = to_fixed_t(uv[2][1]);
    double sf = (l0 * (double)s0 / a.w + l1 * (double)s1 / b.w + l2 * (double)s2 / c.w) / iw;
    double tf = (l0 * (double)t0 / a.w + l1 * (double)t1 / b.w + l2 * (double)t2 / c.w) / iw;
    long s = (long)(GLint)sf, t = (long)(GLint)tf;
    out.near_boundary = near_texel_boundary(s, t);
    out.color = modulate_white(sample_fixed(tex, s, t));
    return out;
}

// ---------------------------------------------------------------------------
struct Snapshot {
    std::vector<Pixel> color;
    std::vector<unsigned short> depth;
};

static Snapshot snapshot(ZBuffer* z) {
    Snapshot s;
    s.color.resize((size_t)W * H);
    s.depth.resize((size_t)W * H);
    const PIXEL* p = (const PIXEL*)z->pbuf;
    for (int i = 0; i < W * H; i++) {
        s.color[i] = unpack((unsigned)p[i] & 0xffffffu);
        s.depth[i] = z->zbuf[i];
    }
    return s;
}

static bool is_black(const Pixel& p) { return p.r == 0 && p.g == 0 && p.b == 0; }

static Pixel pixel_at(ZBuffer* z, int x, int y) {
    return unpack((unsigned)((PIXEL*)z->pbuf)[(size_t)y * W + x] & 0xffffffu);
}

struct Fixture {
    ZBuffer* z = nullptr;
    TestTexture tex;
    Fixture() {
        z = ZB_open(W, H, ZB_MODE_RGBA, 0);
        if (!z) throw std::runtime_error("ZB_open failed");
        glInit(z);
        glViewport(0, 0, W, H);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glColor3f(1, 1, 1);
        glShadeModel(GL_SMOOTH);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        tex.upload();
        glClearColor(0, 0, 0, 0);
    }
    void clear() { glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); }
    Snapshot snap() { return snapshot(z); }
    ~Fixture() {
        glClose();
        ZB_close(z);
    }
};

// ---------------------------------------------------------------------------
// A triangle described in clip space (x,y,z,w) with normalised u,v.
struct ClipTri {
    double x[3], y[3], z[3], w[3];
    double u[3], v[3];
};

// Convert an integer screen target and clip w into clip x/y, choosing a small
// offset so the float projection lands on the target after truncation.
static void screen_target_to_clip(int sx, int sy, double w, const Viewport& vp,
                                  double& cx, double& cy) {
    cx = ((sx + 0.25 - vp.tx) / vp.sx) * w;
    cy = ((sy + 0.25 - vp.ty) / vp.sy) * w;
}

static void apply_frustum() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// Draw a clip-space triangle through glFrustum(-1,1,-1,1,1,10). For that
// matrix clip.x=model.x, clip.y=model.y, clip.w=-model.z, so model.z=-w.
// The clip z seen by the rasterizer is therefore determined by w; the caller
// only needs to supply consistent screen targets.
static void draw_clip_frustum(const ClipTri& t, bool depth_test) {
    if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glTexCoord2f((GLfloat)t.u[i], (GLfloat)t.v[i]);
        glVertex3f((GLfloat)t.x[i], (GLfloat)t.y[i], (GLfloat)(-t.w[i]));
    }
    glEnd();
}

// Identity projection: model == clip, w = 1 (library fast path).
static void draw_clip_identity(const ClipTri& t, bool depth_test) {
    if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glTexCoord2f((GLfloat)t.u[i], (GLfloat)t.v[i]);
        glVertex4f((GLfloat)t.x[i], (GLfloat)t.y[i], (GLfloat)t.z[i], 1.0f);
    }
    glEnd();
}

// Actual clip z for a model vertex under glFrustum(-1,1,-1,1,1,10):
// z_clip = C*model_z + D with C=-(f+n)/(f-n), D=-2fn/(f-n).
static double frustum_clip_z(double w) {
    const double N = 1.0, F = 10.0;
    const double C = -(F + N) / (F - N);
    const double D = -(2 * F * N) / (F - N);
    double model_z = -w;  // as passed by draw_clip_frustum
    return C * model_z + D;
}

// Fill a ClipTri with screen targets and w values; clip z is derived from the
// frustum so the reference is consistent with what was drawn.
static ClipTri make_frustum_tri(const int sx[3], const int sy[3], const double w[3],
                                const double u[3], const double v[3]) {
    ClipTri t;
    Viewport vp(W, H);
    for (int i = 0; i < 3; i++) {
        screen_target_to_clip(sx[i], sy[i], w[i], vp, t.x[i], t.y[i]);
        t.z[i] = frustum_clip_z(w[i]);
        t.w[i] = w[i];
        t.u[i] = u[i];
        t.v[i] = v[i];
    }
    return t;
}

struct CompareResult {
    long checked = 0;
    long bad = 0;
    long missing = 0;
    long bad_depth = 0;
    long bad_cov = 0;
    std::string detail;
};

// Full-frame reference comparison on interior pixels (all barycentric weights
// >= margin). Checks colour (unless textured==false, see below), coverage and
// the depth plane.
static CompareResult compare_reference(ZBuffer* z, const TestTexture* tex, const ClipTri& t,
                                       double margin, int color_tol, bool check_depth) {
    Viewport vp(W, H);
    RasterPoint rp[3];
    for (int i = 0; i < 3; i++) rp[i] = project(vp, t.x[i], t.y[i], t.z[i], t.w[i]);
    double uv[3][2] = {{t.u[0], t.v[0]}, {t.u[1], t.v[1]}, {t.u[2], t.v[2]}};
    CompareResult r;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double l0, l1, l2;
            if (!barycentric(rp[0], rp[1], rp[2], x, y, l0, l1, l2)) continue;
            if (l0 < margin || l1 < margin || l2 < margin) continue;
            Pixel got = pixel_at(z, x, y);
            if (is_black(got)) {
                r.missing++;
                if (r.detail.empty())
                    r.detail = "uncovered (" + std::to_string(x) + "," + std::to_string(y) + ")";
                continue;
            }
            if (tex) {
                RefPixel want = reference_pixel(*tex, rp[0], rp[1], rp[2], uv, x, y);
                if (want.near_boundary) continue;
                r.checked++;
                if (!got.near(want.color, color_tol)) {
                    r.bad++;
                    if (r.detail.empty())
                        r.detail = "(" + std::to_string(x) + "," + std::to_string(y) + ") got " +
                                   got.str() + " want " + want.color.str();
                }
            }
            if (check_depth) {
                // The zbuffer stores the interpolated window depth (fixed
                // point, 14 fraction bits) shifted right by 14.
                double zr = (l0 * rp[0].z + l1 * rp[1].z + l2 * rp[2].z) / (double)(1 << ZB_POINT_Z_FRAC_BITS);
                int got_z = z->zbuf[(size_t)y * W + x];
                if (std::abs((double)got_z - zr) > 2.0) {
                    r.bad_depth++;
                    if (r.detail.empty())
                        r.detail = "depth (" + std::to_string(x) + "," + std::to_string(y) + ") got " +
                                   std::to_string(got_z) + " want " + std::to_string((long)zr);
                }
            }
        }
    }
    return r;
}

// Reference-correct draw helper for the identity projection.
static CompareResult check_identity(const ClipTri& t, double margin, int tol, bool depth) {
    Fixture f;
    f.clear();
    draw_clip_identity(t, false);
    return compare_reference(f.z, &f.tex, t, margin, tol, depth);
}

// Reference-correct draw helper for the frustum projection.
static CompareResult check_frustum(const ClipTri& t, double margin, int tol, bool depth) {
    Fixture f;
    apply_frustum();
    f.clear();
    draw_clip_frustum(t, false);
    return compare_reference(f.z, &f.tex, t, margin, tol, depth);
}

// ---------------------------------------------------------------------------
// Homogeneous clipping reference (Sutherland-Hodgman against the six planes),
// used to check that clip-generated vertices keep perspective-correct uv.
struct CV {
    double x, y, z, w, u, v;
};

static CV lerp_cv(const CV& a, const CV& b, double t) {
    CV r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    return r;
}

static void clip_poly_plane(std::vector<CV>& poly, int plane) {
    std::vector<CV> out;
    size_t n = poly.size();
    for (size_t i = 0; i < n; i++) {
        const CV& a = poly[i];
        const CV& b = poly[(i + 1) % n];
        double da, db;
        switch (plane) {
        case 0: da = a.x + a.w; db = b.x + b.w; break;
        case 1: da = a.w - a.x; db = b.w - b.x; break;
        case 2: da = a.y + a.w; db = b.y + b.w; break;
        case 3: da = a.w - a.y; db = b.w - b.y; break;
        case 4: da = a.z + a.w; db = b.z + b.w; break;
        default: da = a.w - a.z; db = b.w - b.z; break;
        }
        bool ina = da >= 0, inb = db >= 0;
        if (ina) out.push_back(a);
        if (ina != inb) out.push_back(lerp_cv(a, b, da / (da - db)));
    }
    poly.swap(out);
}

static std::vector<CV> clip_polygon(const ClipTri& t) {
    std::vector<CV> poly;
    for (int i = 0; i < 3; i++) poly.push_back(CV{t.x[i], t.y[i], t.z[i], t.w[i], t.u[i], t.v[i]});
    for (int p = 0; p < 6; p++) clip_poly_plane(poly, p);
    return poly;
}

// Compare every interior pixel of the clipped polygon against the rational
// reference. This exercises the vertices created by near/side clipping.
static CompareResult compare_clipped_polygon(ZBuffer* z, const TestTexture& tex,
                                             const std::vector<CV>& poly, double margin,
                                             bool check_depth) {
    CompareResult r;
    Viewport vp(W, H);
    for (size_t ti = 1; ti + 1 < poly.size(); ti++) {
        const CV& A = poly[0];
        const CV& B = poly[ti];
        const CV& C = poly[ti + 1];
        RasterPoint rp[3] = {
            project(vp, A.x, A.y, A.z, A.w),
            project(vp, B.x, B.y, B.z, B.w),
            project(vp, C.x, C.y, C.z, C.w),
        };
        double uv[3][2] = {{A.u, A.v}, {B.u, B.v}, {C.u, C.v}};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double l0, l1, l2;
                if (!barycentric(rp[0], rp[1], rp[2], x, y, l0, l1, l2)) continue;
                if (l0 < margin || l1 < margin || l2 < margin) continue;
                Pixel got = pixel_at(z, x, y);
                if (is_black(got)) {
                    r.missing++;
                    if (r.detail.empty())
                        r.detail = "uncovered (" + std::to_string(x) + "," + std::to_string(y) + ")";
                    continue;
                }
                RefPixel want = reference_pixel(tex, rp[0], rp[1], rp[2], uv, x, y);
                if (want.near_boundary) continue;
                r.checked++;
                if (!got.near(want.color, 0)) {
                    r.bad++;
                    if (r.detail.empty())
                        r.detail = "(" + std::to_string(x) + "," + std::to_string(y) + ") got " +
                                   got.str() + " want " + want.color.str();
                }
                if (check_depth) {
                    double zr = (l0 * rp[0].z + l1 * rp[1].z + l2 * rp[2].z) / (double)(1 << ZB_POINT_Z_FRAC_BITS);
                    int got_z = z->zbuf[(size_t)y * W + x];
                    if (std::abs((double)got_z - zr) > 4.0) r.bad_depth++;
                }
            }
    }
    return r;
}

struct Coverage {
    long count = 0;
    int xmin = 999, xmax = -1, ymin = 999, ymax = -1;
};

static Coverage coverage(ZBuffer* z) {
    Coverage c;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!is_black(pixel_at(z, x, y))) {
                c.count++;
                if (x < c.xmin) c.xmin = x;
                if (x > c.xmax) c.xmax = x;
                if (y < c.ymin) c.ymin = y;
                if (y > c.ymax) c.ymax = y;
            }
    return c;
}

static bool frames_equal(const Snapshot& a, const Snapshot& b, std::string* where = nullptr) {
    for (int i = 0; i < W * H; i++)
        if (!(a.color[i] == b.color[i])) {
            if (where) *where = std::to_string(i % W) + "," + std::to_string(i / W);
            return false;
        }
    return true;
}

}  // namespace

// ===========================================================================
// Group: orthographic
// ===========================================================================
static void test_orthographic(Suite& s) {
    // The exact reproducer from checks/scene.c: only the vertex depth changes
    // between the two passes; the uv mapping must be identical at (12,16).
    {
        Fixture f;
        f.clear();
        ClipTri varied{{-0.75, 0.75, -0.75}, {-0.75, -0.75, 0.75}, {-0.8, 0.8, -0.8},
                       {1, 1, 1}, {0, 1, 0}, {0, 0, 1}};
        draw_clip_identity(varied, false);
        Pixel a = pixel_at(f.z, 12, 16);
        f.clear();
        ClipTri flat{{-0.75, 0.75, -0.75}, {-0.75, -0.75, 0.75}, {0, 0, 0},
                     {1, 1, 1}, {0, 1, 0}, {0, 0, 1}};
        draw_clip_identity(flat, false);
        Pixel b = pixel_at(f.z, 12, 16);
        s.check(a == b, "ortho reproducer differs: varied=" + a.str() + " flat=" + b.str());
        s.check(!is_black(a), "ortho reproducer pixel (12,16) not covered");
        s.check(a == Pixel{0x5f, 0x74, 0x02}, "ortho reproducer texel changed: " + a.str());
    }

    // Full orthographic frame with integer screen vertices spanning several
    // scanline lengths (including the 8-pixel block and its tail). Every
    // interior pixel, the depth plane and the coverage are checked.
    {
        ClipTri t;
        Viewport vp(W, H);
        for (int i = 0; i < 3; i++) t.w[i] = 1.0;
        int SX[3] = {3, 26, 3}, SY[3] = {26, 3, 3};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        for (int i = 0; i < 3; i++) {
            screen_target_to_clip(SX[i], SY[i], 1.0, vp, t.x[i], t.y[i]);
            t.z[i] = (i == 0) ? 0.4 : (i == 1 ? -0.6 : 0.1);
            t.u[i] = U[i];
            t.v[i] = V[i];
        }
        CompareResult r = check_identity(t, 0.02, 0, true);
        s.check(r.bad == 0, "ortho full-frame texture mismatch: " + r.detail +
                                " bad=" + std::to_string(r.bad));
        s.check(r.missing == 0, "ortho full-frame uncovered interior: " + r.detail);
        s.check(r.checked > 200, "ortho full-frame only checked " + std::to_string(r.checked));
        s.check(r.bad_depth == 0, "ortho full-frame depth mismatch: " + r.detail);
        // Widths 24, 23 and 1 pixels: scanlines longer than the 8-pixel block
        // exercise both the blocked loop and its tail. Additionally verify
        // that the flat depth gradient (0.4 .. -0.6) is a straight plane.
        long plane_bad = 0;
        RasterPoint rp[3];
        for (int i = 0; i < 3; i++) rp[i] = project(vp, t.x[i], t.y[i], t.z[i], t.w[i]);
        Fixture f;
        f.clear();
        draw_clip_identity(t, false);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double l0, l1, l2;
                if (!barycentric(rp[0], rp[1], rp[2], x, y, l0, l1, l2)) continue;
                if (l0 < 0.02 || l1 < 0.02 || l2 < 0.02) continue;
                if (is_black(pixel_at(f.z, x, y))) continue;
                double zr = (l0 * rp[0].z + l1 * rp[1].z + l2 * rp[2].z) /
                            (double)(1 << ZB_POINT_Z_FRAC_BITS);
                if (std::abs((double)f.z->zbuf[(size_t)y * W + x] - zr) > 2.0) plane_bad++;
            }
        s.check(plane_bad == 0, "ortho depth plane not linear (" + std::to_string(plane_bad) + " px)");
    }

    // Changing clip z while x, y and w stay fixed must not change any colour.
    // Use a custom projection that scales z only (m[2][2]=0.5), leaving the
    // w row zero so the library's W=1 fast path applies.
    {
        Fixture f;
        GLfloat P[16] = {1, 0, 0, 0,
                         0, 1, 0, 0,
                         0, 0, 0.5f, 0,
                         0, 0, 0, 1};
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(P);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        auto draw = [&](double z) {
            f.clear();
            glBegin(GL_TRIANGLES);
            glTexCoord2f(0, 0);
            glVertex3f(-0.75f, -0.75f, (GLfloat)z);
            glTexCoord2f(1, 0);
            glVertex3f(0.75f, -0.75f, (GLfloat)z);
            glTexCoord2f(0, 1);
            glVertex3f(-0.75f, 0.75f, (GLfloat)z);
            glEnd();
        };
        draw(-0.9);
        Snapshot s1 = f.snap();
        draw(0.9);
        Snapshot s2 = f.snap();
        bool same = true;
        std::string at;
        for (int i = 0; i < W * H; i++)
            if (!(s1.color[i] == s2.color[i])) {
                same = false;
                at = std::to_string(i % W) + "," + std::to_string(i / W);
                break;
            }
        s.check(same, "clip-z-only change altered colour at " + at);
        bool depth_diff = false;
        for (int i = 0; i < W * H; i++)
            if (s1.depth[i] != s2.depth[i]) depth_diff = true;
        s.check(depth_diff, "clip-z-only change did not affect depth");
        // And the colour equals the per-pixel reference (w=1, z irrelevant).
        Viewport vp(W, H);
        ClipTri t;
        for (int i = 0; i < 3; i++) t.w[i] = 1.0;
        t.x[0] = -0.75; t.y[0] = -0.75; t.z[0] = 0;
        t.x[1] = 0.75; t.y[1] = -0.75; t.z[1] = 0;
        t.x[2] = -0.75; t.y[2] = 0.75; t.z[2] = 0;
        t.u[0] = 0; t.u[1] = 1; t.u[2] = 0;
        t.v[0] = 0; t.v[1] = 0; t.v[2] = 1;
        CompareResult r = compare_reference(f.z, &f.tex, t, 0.02, 0, false);
        s.check(r.bad == 0, "clip-z-only colour vs reference: " + r.detail);
        (void)vp;
    }
}

// ===========================================================================
// Group: perspective
// ===========================================================================
static void test_perspective(Suite& s) {
    // Strong perspective through glFrustum: per-vertex w 1.2 .. 6.0.
    {
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        CompareResult r = check_frustum(t, 0.02, 0, true);
        s.check(r.bad == 0, "perspective texture mismatch: " + r.detail +
                                " bad=" + std::to_string(r.bad));
        s.check(r.checked > 200, "perspective only checked " + std::to_string(r.checked));
        s.check(r.missing == 0, "perspective uncovered interior: " + r.detail);
        s.check(r.bad_depth == 0, "perspective depth mismatch: " + r.detail);
    }

    // The rational sample must differ from a plain affine interpolation of
    // the same vertices, and match the rational reference. This proves the
    // correction is actually applied.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        f.clear();
        draw_clip_frustum(t, false);
        Viewport vp(W, H);
        RasterPoint rp[3];
        for (int i = 0; i < 3; i++) rp[i] = project(vp, t.x[i], t.y[i], t.z[i], t.w[i]);
        // Find an interior pixel where affine and rational samples differ.
        double uv[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        int px = -1, py = -1;
        Pixel affine{0, 0, 0}, rational{0, 0, 0};
        for (int y = 4; y < H - 4 && px < 0; y++)
            for (int x = 4; x < W - 4; x++) {
                double l0, l1, l2;
                if (!barycentric(rp[0], rp[1], rp[2], x, y, l0, l1, l2)) continue;
                if (l0 < 0.2 || l1 < 0.2 || l2 < 0.2) continue;
                double ua = l0 * U[0] + l1 * U[1] + l2 * U[2];
                double va = l0 * V[0] + l1 * V[1] + l2 * V[2];
                Pixel pa = modulate_white(sample_fixed(f.tex, to_fixed_s(ua), to_fixed_t(va)));
                RefPixel rpix = reference_pixel(f.tex, rp[0], rp[1], rp[2], uv, x, y);
                if (rpix.near_boundary) continue;
                if (!(pa == rpix.color)) {
                    px = x; py = y; affine = pa; rational = rpix.color;
                    break;
                }
            }
        s.check(px >= 0, "could not find a pixel where affine differs from rational");
        if (px >= 0) {
            Pixel got = pixel_at(f.z, px, py);
            s.check(got == rational, "rational sample mismatch at (" + std::to_string(px) + "," +
                                         std::to_string(py) + "): got " + got.str() + " want " +
                                         rational.str());
            s.check(!(got == affine), "sample at chosen pixel equals affine interpolation");
        }
    }

    // A second frustum scene with a large w spread (far vertex w=8) checks
    // that the correction stays accurate when 1/w varies by nearly an order
    // of magnitude. It also spans scanlines much longer than the 8-pixel
    // internal block (up to 30 pixels) so the block tail is exercised.
    {
        int SX[3] = {1, 30, 1};
        int SY[3] = {30, 12, 2};
        double WV[3] = {1.0, 8.0, 2.0};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        CompareResult r = check_frustum(t, 0.02, 0, true);
        s.check(r.bad == 0, "strong perspective mismatch: " + r.detail);
        s.check(r.checked > 100, "strong perspective only checked " + std::to_string(r.checked));
        s.check(r.missing == 0, "strong perspective uncovered interior: " + r.detail);
    }

    // A zero depth gradient across a strongly perspective face: use a custom
    // projection with the z row zero and the w row [0,0,-1,0]. Then clip z is
    // identically 0 (window depth constant) while clip w = -model.z varies,
    // so the texture must still be perspective corrected.
    {
        Fixture f;
        // glLoadMatrixf takes column-major input; this matrix maps
        // (x,y,z,1) -> (x, y, 0, -z): zero clip z (constant window depth)
        // and w = -model.z.
        GLfloat P[16] = {1, 0, 0, 0,
                         0, 1, 0, 0,
                         0, 0, 0, -1,
                         0, 0, 0, 0};
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(P);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        int SX[3] = {3, 28, 3};
        int SY[3] = {28, 26, 3};
        double WV[3] = {2.0, 5.0, 2.0};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        Viewport vp(W, H);
        ClipTri t;
        for (int i = 0; i < 3; i++) {
            screen_target_to_clip(SX[i], SY[i], WV[i], vp, t.x[i], t.y[i]);
            t.z[i] = 0.0;
            t.w[i] = WV[i];
            t.u[i] = U[i];
            t.v[i] = V[i];
        }
        f.clear();
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; i++) {
            glTexCoord2f((GLfloat)t.u[i], (GLfloat)t.v[i]);
            glVertex3f((GLfloat)t.x[i], (GLfloat)t.y[i], (GLfloat)(-t.w[i]));
        }
        glEnd();
        CompareResult r = compare_reference(f.z, &f.tex, t, 0.02, 0, true);
        s.check(r.bad == 0 && r.missing == 0, "zero-gradient perspective mismatch: " + r.detail);
        s.check(r.bad_depth == 0, "zero-gradient depth not constant");
        int zmin = 0xffff, zmax = -1;
        long checked = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (!is_black(pixel_at(f.z, x, y))) {
                    checked++;
                    int zv = f.z->zbuf[(size_t)y * W + x];
                    if (zv < zmin) zmin = zv;
                    if (zv > zmax) zmax = zv;
                }
        s.check(checked > 200, "zero-gradient scene too small: " + std::to_string(checked));
        s.check(zmax == zmin, "zero-gradient depth spread not zero: " + std::to_string(zmax - zmin));
    }

    // Tiny (but non-zero) depth gradient: w barely differs, so the window
    // depth spread is small while the rational texture weights still vary.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {3, 28, 3};
        int SY[3] = {28, 26, 3};
        double WV[3] = {2.0, 2.001, 2.0};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        f.clear();
        draw_clip_frustum(t, false);
        CompareResult r = compare_reference(f.z, &f.tex, t, 0.02, 0, true);
        s.check(r.bad == 0 && r.missing == 0, "tiny-gradient perspective mismatch: " + r.detail);
        s.check(r.bad_depth == 0, "tiny-gradient depth plane mismatch");
        int zmin = 0xffff, zmax = -1;
        long checked = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (!is_black(pixel_at(f.z, x, y))) {
                    checked++;
                    int zv = f.z->zbuf[(size_t)y * W + x];
                    if (zv < zmin) zmin = zv;
                    if (zv > zmax) zmax = zv;
                }
        s.check(checked > 200, "tiny-gradient scene too small: " + std::to_string(checked));
        s.check(zmax - zmin > 0 && zmax - zmin < 32,
                "tiny-gradient depth spread out of range: " + std::to_string(zmax - zmin));
    }

    // Strong perspective, per-axis bound: for every covered interior pixel the
    // sampled source texel must be at most one texel away on each axis from
    // the independent per-pixel quotient. This is the precision requirement
    // for the rational interpolation (the implementation evaluates it exactly,
    // so the bound should actually hold with distance 0 in practice; allowing
    // 1 covers nearest ties without weakening the check).
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {1, 30, 2};
        int SY[3] = {30, 29, 1};
        double WV[3] = {1.0, 9.0, 1.3};
        double U[3] = {0.05, 0.95, 0.1}, V[3] = {0.1, 0.05, 0.9};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        f.clear();
        draw_clip_frustum(t, false);
        Viewport vp(W, H);
        RasterPoint rp[3];
        for (int i = 0; i < 3; i++) rp[i] = project(vp, t.x[i], t.y[i], t.z[i], t.w[i]);
        double uv[3][2] = {{t.u[0], t.v[0]}, {t.u[1], t.v[1]}, {t.u[2], t.v[2]}};
        long checked = 0, bound_bad = 0;
        int worst = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double l0, l1, l2;
                if (!barycentric(rp[0], rp[1], rp[2], x, y, l0, l1, l2)) continue;
                if (l0 < 0.02 || l1 < 0.02 || l2 < 0.02) continue;
                Pixel got = pixel_at(f.z, x, y);
                if (is_black(got)) continue;
                // Independent per-pixel quotient on the raw (unquantised)
                // texture coordinates, sampled with the same nearest rule.
                double iw = l0 / t.w[0] + l1 / t.w[1] + l2 / t.w[2];
                double u = (l0 * t.u[0] / t.w[0] + l1 * t.u[1] / t.w[1] + l2 * t.u[2] / t.w[2]) / iw;
                double v = (l0 * t.v[0] / t.w[0] + l1 * t.v[1] / t.w[1] + l2 * t.v[2] / t.w[2]) / iw;
                // Decode the drawn source texel from the colour: each channel
                // is the texel coordinate scaled by the white fixed factor,
                // which is monotone; recover via a linear search over the
                // known procedural texture.
                int gx = -1, gy = -1;
                for (int ty = 0; ty < TEX_DIM && gx < 0; ty++)
                    for (int tx = 0; tx < TEX_DIM; tx++)
                        if (modulate_white(f.tex.texel(tx, ty)) == got) {
                            gx = tx;
                            gy = ty;
                            break;
                        }
                if (gx < 0) {
                    bound_bad++;
                    continue;
                }
                int rx = (int)std::floor(u * TEX_DIM);
                int ry = (int)std::floor(v * TEX_DIM);
                // Pick the nearest such reference texel allowing for the
                // repeat wrap.
                auto adist = [&](int a, int b) {
                    int d = std::abs(a - b);
                    return d < TEX_DIM - d ? d : TEX_DIM - d;
                };
                int d = std::max(adist(gx, rx), adist(gy, ry));
                // Distance in texels of the recovered vs the reference.
                worst = std::max(worst, d);
                checked++;
                if (d > 1) bound_bad++;
            }
        s.check(checked > 200, "per-texel bound only checked " + std::to_string(checked));
        s.check(bound_bad == 0, "per-texel bound exceeded by " + std::to_string(bound_bad) +
                                    " pixels (worst " + std::to_string(worst) + ")");
        s.check(worst <= 1, "per-texel worst distance " + std::to_string(worst));
    }
}

// ===========================================================================
// Group: clipped
// ===========================================================================
static void test_clipped(Suite& s) {
    // Near-plane clip: the first vertex is behind the near plane. The
    // clipping reference produces new vertices whose uv/w must be used with
    // the same rational formula.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {8, 26, 6};
        int SY[3] = {16, 26, 4};
        double WV[3] = {0.6, 4.0, 1.4};  // w=0.6 vertex is behind near (z_clip < -w)
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        // Verify the first vertex really is clipped (z_clip + w < 0).
        s.check(t.z[0] + t.w[0] < 0.0, "near-clip test vertex not actually behind the near plane");
        f.clear();
        draw_clip_frustum(t, false);
        std::vector<CV> poly = clip_polygon(t);
        s.check(poly.size() >= 4, "near-clip reference polygon degenerated to " + std::to_string(poly.size()) + " vertices");
        CompareResult r = compare_clipped_polygon(f.z, f.tex, poly, 0.05, false);
        s.check(r.bad == 0, "near-clip texture mismatch: " + r.detail +
                                " bad=" + std::to_string(r.bad));
        s.check(r.checked > 30, "near-clip only checked " + std::to_string(r.checked));
        s.check(r.missing == 0, "near-clip uncovered interior: " + r.detail);
    }

    // Side-plane clip: a vertex far to the left (x < -w). The visible region
    // stems from a clip-generated edge and must use the rational formula.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {-30, 28, 4};
        int SY[3] = {16, 26, 4};
        double WV[3] = {2.0, 5.0, 1.6};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        s.check(t.x[0] + t.w[0] < 0.0, "side-clip test vertex not actually outside x >= -w");
        f.clear();
        draw_clip_frustum(t, false);
        std::vector<CV> poly = clip_polygon(t);
        CompareResult r = compare_clipped_polygon(f.z, f.tex, poly, 0.05, false);
        s.check(r.bad == 0, "side-clip texture mismatch: " + r.detail);
        s.check(r.checked > 20, "side-clip only checked " + std::to_string(r.checked));
        Coverage cov = coverage(f.z);
        s.check(cov.count > 0 && cov.xmin <= 2, "side-clip did not reach the left border");
    }

    // Two vertices outside the same side plane: the surviving piece is a
    // single clip-generated triangle.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {-24, -18, 26};
        int SY[3] = {10, 26, 18};
        double WV[3] = {3.0, 2.0, 6.0};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        f.clear();
        draw_clip_frustum(t, false);
        std::vector<CV> poly = clip_polygon(t);
        s.check(poly.size() >= 3, "two-outside clip produced no polygon");
        CompareResult r = compare_clipped_polygon(f.z, f.tex, poly, 0.05, false);
        s.check(r.bad == 0, "two-outside clip texture mismatch: " + r.detail);
        s.check(r.missing == 0 || r.checked == 0, "two-outside clip uncovered interior");
    }
}

// ===========================================================================
// Group: states
// ===========================================================================
static void test_states(Suite& s) {
    // Blend on/off: with GL_ONE/GL_ZERO both paths must produce the same
    // perspective-correct frame, matching the reference.
    {
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);

        Fixture f;
        apply_frustum();
        f.clear();
        draw_clip_frustum(t, false);  // NOBLEND
        CompareResult rn = compare_reference(f.z, &f.tex, t, 0.1, 0, false);
        Snapshot sn = f.snap();

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ZERO);
        f.clear();
        draw_clip_frustum(t, false);  // blend path
        CompareResult rb = compare_reference(f.z, &f.tex, t, 0.1, 0, false);
        Snapshot sb = f.snap();
        glDisable(GL_BLEND);

        s.check(rn.bad == 0 && rn.missing == 0, "NOBLEND reference mismatch: " + rn.detail);
        s.check(rb.bad == 0 && rb.missing == 0, "blend reference mismatch: " + rb.detail);
        std::string at;
        s.check(frames_equal(sn, sb, &at), "blend and NOBLEND frames differ at " + at);
        s.check(rn.checked > 100, "blend/NOBLEND checked too few pixels: " + std::to_string(rn.checked) + "/" + std::to_string(rb.checked));
        s.check(std::labs(rn.checked - rb.checked) <= 2, "blend/NOBLEND checked counts differ");
    }

    // TGL_NO_DRAW_COLOR texels are discarded in place, and must not write
    // depth, so a later background shows through the hole but not through
    // opaque texels.
    {
        Fixture f;
        TestTexture hole_tex;
        // The quad covers the viewport with uv 0..1, so texture columns
        // 128..191 land on the right half of the screen. Mark them transparent.
        for (int y = 0; y < TEX_DIM; y++)
            for (int x = 128; x < 192; x++) {
                unsigned char* p = &hole_tex.rgb[((size_t)y * TEX_DIM + x) * 3];
                p[0] = (TGL_NO_DRAW_COLOR >> 16) & 0xff;
                p[1] = (TGL_NO_DRAW_COLOR >> 8) & 0xff;
                p[2] = TGL_NO_DRAW_COLOR & 0xff;
            }
        hole_tex.upload();
        glBindTexture(GL_TEXTURE_2D, hole_tex.id);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        f.clear();
        auto quad = [&](double z) {
            glBegin(GL_TRIANGLES);
            glTexCoord2f(0, 0); glVertex3f(-1, -1, (GLfloat)z);
            glTexCoord2f(1, 0); glVertex3f(1, -1, (GLfloat)z);
            glTexCoord2f(1, 1); glVertex3f(1, 1, (GLfloat)z);
            glTexCoord2f(0, 0); glVertex3f(-1, -1, (GLfloat)z);
            glTexCoord2f(1, 1); glVertex3f(1, 1, (GLfloat)z);
            glTexCoord2f(0, 1); glVertex3f(-1, 1, (GLfloat)z);
            glEnd();
        };
        quad(-0.5);
        int hx = 22, hy = 16, ox = 8, oy = 16;
        Pixel hole = pixel_at(f.z, hx, hy);
        s.check(is_black(hole), "transparent texel drawn at " + std::to_string(hx) + "," +
                                    std::to_string(hy) + " -> " + hole.str());
        s.check(f.z->zbuf[(size_t)hy * W + hx] == 0, "transparent texel wrote depth");
        s.check(!is_black(pixel_at(f.z, ox, oy)), "opaque texel not drawn at " + std::to_string(ox) + "," +
                                                       std::to_string(oy));
        // Background far behind: shows through the hole, hidden elsewhere.
        glDisable(GL_TEXTURE_2D);
        glColor3f(0, 1, 0);
        glEnable(GL_DEPTH_TEST);
        quad(0.5);
        Pixel bg = pixel_at(f.z, hx, hy);
        s.check(bg.g > 0x80 && bg.r == 0 && bg.b == 0,
                "background did not show through transparent hole: " + bg.str());
        Pixel solid = pixel_at(f.z, ox, oy);
        s.check(!(solid.g > 0x80 && solid.r == 0), "background showed through an opaque texel");
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, f.tex.id);
    }

    // Polygon stipple holes are discarded in place and do not write depth.
    {
        Fixture f;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        f.clear();
        unsigned char pat[128];
        std::memset(pat, 0xFF, sizeof pat);
        pat[8 * 4 + 1] = 0x00;  // row y=8, x 8..15 discarded
        glPolygonStipple(pat);
        glEnable(GL_POLYGON_STIPPLE);
        glColor3f(1, 1, 1);
        glBegin(GL_TRIANGLES);
        glVertex3f(-1, -1, 0);
        glVertex3f(1, -1, 0);
        glVertex3f(-1, 1, 0);
        glVertex3f(1, -1, 0);
        glVertex3f(1, 1, 0);
        glVertex3f(-1, 1, 0);
        glEnd();
        glDisable(GL_POLYGON_STIPPLE);
        s.check(is_black(pixel_at(f.z, 10, 8)), "stipple hole was filled at (10,8)");
        s.check(!is_black(pixel_at(f.z, 10, 9)), "stipple outside the hole is empty at (10,9)");
        s.check(f.z->zbuf[(size_t)8 * W + 10] == 0, "stipple hole wrote depth");
        glColor3f(1, 0, 0);
        glEnable(GL_DEPTH_TEST);
        glBegin(GL_TRIANGLES);
        glVertex3f(-1, -1, 0.5f);
        glVertex3f(1, -1, 0.5f);
        glVertex3f(-1, 1, 0.5f);
        glVertex3f(1, -1, 0.5f);
        glVertex3f(1, 1, 0.5f);
        glVertex3f(-1, 1, 0.5f);
        glEnd();
        Pixel hole = pixel_at(f.z, 10, 8);
        s.check(hole.r > 0x80 && hole.g == 0, "background did not show through stipple hole: " + hole.str());
        Pixel solid = pixel_at(f.z, 10, 9);
        s.check(!(solid.r > 0x80 && solid.g == 0), "background showed through a stippled pixel");
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, f.tex.id);
    }

    // Switching textures must not carry over any per-face perspective state.
    {
        Fixture f;
        TestTexture alt;
        for (int y = 0; y < TEX_DIM; y++)
            for (int x = 0; x < TEX_DIM; x++) {
                unsigned char* p = &alt.rgb[((size_t)y * TEX_DIM + x) * 3];
                p[0] = (unsigned char)(255 - x);
                p[1] = (unsigned char)(255 - y);
                p[2] = 7;
            }
        alt.upload();
        apply_frustum();
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        glBindTexture(GL_TEXTURE_2D, alt.id);
        f.clear();
        draw_clip_frustum(t, false);
        CompareResult r = compare_reference(f.z, &alt, t, 0.1, 0, false);
        s.check(r.bad == 0, "switched-texture mismatch: " + r.detail);
        s.check(r.checked > 100, "switched texture only checked " + std::to_string(r.checked));
        s.check(is_black(pixel_at(f.z, 0, 0)), "switched texture bled into the corner");
    }

    // Switching the projection between two faces must not reuse the first
    // face's perspective quantity: the second face must match a fresh-context
    // render of the same geometry, and a different projection on the same
    // geometry must give a different picture.
    {
        Viewport vp(W, H);
        // Face A: orthographic (identity) projection, w = 1.
        ClipTri a;
        int ax[3] = {4, 27, 4}, ay[3] = {27, 27, 4};
        for (int i = 0; i < 3; i++) {
            a.w[i] = 1.0;
            screen_target_to_clip(ax[i], ay[i], 1.0, vp, a.x[i], a.y[i]);
            a.z[i] = 0.0;
            a.u[i] = (i == 1) ? 1 : 0;
            a.v[i] = (i == 2) ? 1 : 0;
        }
        // Face B: frustum projection, strong perspective.
        int bx[3] = {4, 27, 4}, by[3] = {27, 27, 4};
        double bw[3] = {1.2, 6.0, 1.5};
        ClipTri b = make_frustum_tri(bx, by, bw, a.u, a.v);

        // The library has one global context, so use a single Fixture at a
        // time and snapshot between phases.
        Snapshot after_switch, fresh, ortho;
        {
            Fixture f;
            f.clear();
            draw_clip_identity(a, false);  // face A: no per-face perspective state
            apply_frustum();
            f.clear();
            draw_clip_frustum(b, false);  // face B
            after_switch = f.snap();
            CompareResult rb = compare_reference(f.z, &f.tex, b, 0.1, 0, false);
            s.check(rb.bad == 0 && rb.checked > 100, "face B after switch mismatch: " + rb.detail);
        }
        {
            Fixture g;
            apply_frustum();
            g.clear();
            draw_clip_frustum(b, false);  // fresh context, face B only
            fresh = g.snap();
        }
        {
            Fixture h;
            h.clear();
            draw_clip_identity(a, false);
            ortho = h.snap();
        }
        std::string at;
        s.check(frames_equal(after_switch, fresh, &at),
                "projection switch leaked state into face B at " + at);
        bool differs = false;
        for (int i = 0; i < W * H; i++)
            if (!(ortho.color[i] == fresh.color[i])) differs = true;
        s.check(differs, "orthographic and perspective renders were identical");
    }
}

// ===========================================================================
// Group: compatibility
// ===========================================================================
static void test_compatibility(Suite& s) {
    // Untextured flat colour and depth direction/equality rules are kept.
    {
        Fixture f;
        glDisable(GL_TEXTURE_2D);
        glShadeModel(GL_FLAT);
        glEnable(GL_DEPTH_TEST);
        f.clear();
        // Draw far (clip z=+0.8) then near (clip z=-0.8) in two orders; the
        // near triangle must win both times (window z is larger for z=-0.8).
        auto tri = [&](double z, double r, double g, double b) {
            glColor3f((GLfloat)r, (GLfloat)g, (GLfloat)b);
            glBegin(GL_TRIANGLES);
            glVertex3f(-0.8f, -0.8f, (GLfloat)z);
            glVertex3f(0.8f, -0.8f, (GLfloat)z);
            glVertex3f(-0.8f, 0.8f, (GLfloat)z);
            glEnd();
        };
        tri(0.8, 1, 0, 0);   // far, red
        tri(-0.8, 0, 1, 0);  // near, green
        Pixel p = pixel_at(f.z, 10, 10);
        s.check(p.g > 0x80 && p.r == 0, "near triangle did not win (far-first): " + p.str());
        f.clear();
        tri(-0.8, 0, 1, 0);
        tri(0.8, 1, 0, 0);
        p = pixel_at(f.z, 10, 10);
        s.check(p.g > 0x80 && p.r == 0, "near triangle did not win (near-first): " + p.str());
        // Equal depth: the later triangle wins (z >= zpix).
        f.clear();
        tri(0.0, 0, 1, 0);
        tri(0.0, 1, 0, 0);
        p = pixel_at(f.z, 10, 10);
        s.check(p.r > 0x80 && p.g == 0, "equal-depth later triangle did not win: " + p.str());
        // Flat colour equals the current colour modulation.
        f.clear();
        tri(0.0, 0.25, 0.5, 0.75);
        unsigned fr = color_fixed(0.25), fg = color_fixed(0.5), fb = color_fixed(0.75);
        Pixel want = color_pixel(fr, fg, fb);
        Pixel got = pixel_at(f.z, 10, 10);
        s.check(got.near(want, 1), "flat colour changed: got " + got.str() + " want " + want.str());
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, f.tex.id);
    }

    // Smooth colour interpolation is unchanged: interior pixels lie between
    // the endpoint colours and differ from each other.
    {
        Fixture f;
        glDisable(GL_TEXTURE_2D);
        glShadeModel(GL_SMOOTH);
        glDisable(GL_DEPTH_TEST);
        f.clear();
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0);
        glVertex3f(-0.8f, -0.8f, 0);
        glColor3f(0, 1, 0);
        glVertex3f(0.8f, -0.8f, 0);
        glColor3f(0, 0, 1);
        glVertex3f(-0.8f, 0.8f, 0);
        glEnd();
        // The vertex at (-0.8,+0.8) has colour blue; the one at (-0.8,-0.8)
        // is red (bottom) and the one at (+0.8,-0.8) is green. Interpolation
        // must make the sampled colours differ and stay clamped.
        Pixel top = pixel_at(f.z, 5, 5);   // near the blue vertex
        Pixel bot = pixel_at(f.z, 5, 21);  // near the red vertex
        s.check(!is_black(top) && !is_black(bot), "smooth colour samples not covered");
        s.check(!(top == bot), "smooth colour interpolation produced identical pixels");
        s.check(top.b > bot.b, "smooth colour blue does not increase toward the blue vertex");
        s.check(bot.r > top.r, "smooth colour red does not increase toward the red vertex");
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, f.tex.id);
    }

    // Lit textured triangle: with a unit white light and white diffuse
    // material the lit colour is white, so the pixels must equal the plain
    // white-modulated rational reference.
    {
        Fixture f;
        apply_frustum();
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glDisable(GL_COLOR_MATERIAL);
        float zero[4] = {0, 0, 0, 1};
        float one[4] = {1, 1, 1, 1};
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, one);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
        glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, one);
        float pos[4] = {0, 0, 1, 0};
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        f.clear();
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; i++) {
            glNormal3f(0, 0, 1);
            glTexCoord2f((GLfloat)t.u[i], (GLfloat)t.v[i]);
            glVertex3f((GLfloat)t.x[i], (GLfloat)t.y[i], (GLfloat)(-t.w[i]));
        }
        glEnd();
        CompareResult r = compare_reference(f.z, &f.tex, t, 0.1, 0, false);
        s.check(r.bad == 0, "lit textured rational mismatch: " + r.detail);
        s.check(r.checked > 100, "lit textured only checked " + std::to_string(r.checked));
        s.check(!is_black(pixel_at(f.z, 12, 16)), "lit textured pixel not covered");
    }

    // Display list replay: identical to a direct draw, and replaying after a
    // different face/projection must not inherit any stale perspective.
    {
        Fixture f;
        apply_frustum();
        int SX[3] = {2, 28, 2};
        int SY[3] = {28, 27, 2};
        double WV[3] = {1.2, 6.0, 1.5};
        double U[3] = {0, 1, 0}, V[3] = {0, 0, 1};
        ClipTri t = make_frustum_tri(SX, SY, WV, U, V);
        GLuint list = glGenLists(1);
        glNewList(list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; i++) {
            glTexCoord2f((GLfloat)t.u[i], (GLfloat)t.v[i]);
            glVertex3f((GLfloat)t.x[i], (GLfloat)t.y[i], (GLfloat)(-t.w[i]));
        }
        glEnd();
        glEndList();
        // Direct draw.
        f.clear();
        draw_clip_frustum(t, false);
        Snapshot direct = f.snap();
        // Draw a differently-projected face, then replay the list.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        f.clear();
        glBegin(GL_TRIANGLES);
        glTexCoord2f(0, 0); glVertex3f(-0.5f, -0.5f, 0);
        glTexCoord2f(1, 0); glVertex3f(0.5f, -0.5f, 0);
        glTexCoord2f(0, 1); glVertex3f(-0.5f, 0.5f, 0);
        glEnd();
        apply_frustum();
        f.clear();
        glCallList(list);
        Snapshot replay = f.snap();
        std::string at;
        s.check(frames_equal(direct, replay, &at), "display list replay differs from direct at " + at);
        CompareResult r = compare_reference(f.z, &f.tex, t, 0.1, 0, false);
        s.check(r.bad == 0, "display list replay reference mismatch: " + r.detail);
        s.check(r.checked > 100, "display list replay only checked " + std::to_string(r.checked));
    }
}

// ===========================================================================
void register_tests(Suite& s) {
    s.add("orthographic", test_orthographic);
    s.add("perspective", test_perspective);
    s.add("clipped", test_clipped);
    s.add("states", test_states);
    s.add("compatibility", test_compatibility);
}
