// Task target: analytic varying derivatives, clipping/primitive coverage,
// and pglTexture2DVarying sampling / invalid contexts.
//
// Every numeric expectation is recomputed independently in double precision
// from the clip coordinates, w, the glViewport mapping and the same snapped
// integer edge equations the rasterizer uses: double barycentrics plus the
// perspective quotient derivative (or the screen-affine derivative).

#include "task_support/checks.hpp"

#include <portablegl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kMaxC = GL_MAX_VERTEX_OUTPUT_COMPONENTS;

void expect(Check& ck, bool ok, const std::string& msg) {
    ck(ok, msg.c_str());
}

bool dclose(double got, double ref, double abs_eps = 1e-4, double rel_eps = 2e-4) {
    return std::fabs(got - ref) <= abs_eps + rel_eps * std::fabs(ref);
}

// ----------------------------------------------------------------------------
// Fragment recording

struct FragRec {
    int ix = 0, iy = 0;
    int valid = 0;
    float in[kMaxC]{};
    float dx[kMaxC]{};
    float dy[kMaxC]{};
    float col[4] = {1, 1, 1, 1};
};

std::vector<FragRec> g_frags;
int g_record_n = 8;
bool g_record_color = false;

GLuint g_tex = 0;
GLuint g_uslot = 1;
GLuint g_vslot = 4;

struct BadRec {
    int valid = -1;
    int nonzero = -1; // number of nonzero derivative entries
    float col[4]{};
};
std::vector<BadRec> g_bad;

void record_frag(float* in, Shader_Builtins* b, void*) {
    FragRec r;
    r.ix = static_cast<int>(b->gl_FragCoord.x);
    r.iy = static_cast<int>(b->gl_FragCoord.y);
    r.valid = b->pgl_DerivativesValid ? 1 : 0;
    for (int i = 0; i < g_record_n && i < kMaxC; ++i) r.in[i] = in[i];
    for (int i = 0; i < kMaxC; ++i) {
        r.dx[i] = b->pgl_Ddx[i];
        r.dy[i] = b->pgl_Ddy[i];
    }
    if (g_record_color) {
        vec4 c = pglTexture2DVarying(g_tex, in, b, g_uslot, g_vslot);
        r.col[0] = c.x; r.col[1] = c.y; r.col[2] = c.z; r.col[3] = c.w;
        b->gl_FragColor = c;
    } else {
        b->gl_FragColor = make_v4(1, 1, 1, 1);
    }
    g_frags.push_back(r);
}

// Records only the invalid-context contract (used by point/line/fullscreen).
void bad_varying_fs(float* in, Shader_Builtins* b, void*) {
    BadRec r;
    r.valid = b->pgl_DerivativesValid ? 1 : 0;
    int nz = 0;
    for (int i = 0; i < kMaxC; ++i) {
        if (b->pgl_Ddx[i] != 0.0f || b->pgl_Ddy[i] != 0.0f) ++nz;
    }
    r.nonzero = nz;
    vec4 c = pglTexture2DVarying(g_tex, in, b, 0, 1);
    r.col[0] = c.x; r.col[1] = c.y; r.col[2] = c.z; r.col[3] = c.w;
    g_bad.push_back(r);
    b->gl_FragColor = c;
}

// 8 outputs packed as a0=position, a1=out0..3, a2=out4..7.
void pack8_vs(float* out, vec4* a, Shader_Builtins* b, void*) {
    b->gl_Position = a[0];
    out[0] = a[1].x; out[1] = a[1].y; out[2] = a[1].z; out[3] = a[1].w;
    out[4] = a[2].x; out[5] = a[2].y; out[6] = a[2].z; out[7] = a[2].w;
}

void setup_packed_attribs() {
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)(8 * sizeof(float)));
}

GLuint make_record_program(int n, const GLenum* interp, frag_func fs = record_frag) {
    GLuint p = pglCreateProgram(pack8_vs, fs, n, const_cast<GLenum*>(interp), GL_FALSE);
    glUseProgram(p);
    return p;
}

void clear_gl_errors() {
    while (glGetError() != GL_NO_ERROR) {}
}

// ----------------------------------------------------------------------------
// Independent double reference

typedef int64_t i64;

i64 ref_edge(int ax, int ay, int bx, int by, int px, int py) {
    return (i64)(ay - by) * px + (i64)(bx - ax) * py
         + (i64)ax * by - (i64)bx * ay;
}

int ref_snap(double v) { return static_cast<int>(std::floor(v * 256.0 + 0.5)); }

struct RefTri {
    int n = 0;
    int interp[kMaxC]{};
    int provoke = 2;
    int vx[3]{}, vy[3]{};
    double w[3]{}, val[3][kMaxC]{};
    i64 e01v2 = 1, e20v1 = 1, e12v0 = 1;
    double adx = 0, ady = 0, bdx = 0, bdy = 0, gdx = 0, gdy = 0;
    double iw[3]{}, Ddx = 0, Ddy = 0;

    void init(double vw, double vh, double vp_x, double vp_y,
              const double ndc[3][2], const double ww[3],
              const std::vector<float>& vertices, // 3 * 12 packed
              int nn, const GLenum* im, int prov) {
        n = nn;
        provoke = prov;
        for (int i = 0; i < n; ++i) interp[i] = im[i];

        double r = vp_x + vw - 0.01;
        double t = vp_y + vh - 0.01;
        double sx = (r - vp_x) / 2.0, tx = (vp_x + r) / 2.0;
        double sy = (t - vp_y) / 2.0, ty = (vp_y + t) / 2.0;

        for (int k = 0; k < 3; ++k) {
            w[k] = ww[k];
            iw[k] = 1.0 / ww[k];
            double wx = sx * ndc[k][0] + tx;
            double wy = sy * ndc[k][1] + ty;
            vx[k] = ref_snap(wx);
            vy[k] = ref_snap(wy);
            for (int i = 0; i < n; ++i) val[k][i] = vertices[k * 12 + 4 + i];
        }
        e01v2 = ref_edge(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
        e20v1 = ref_edge(vx[2], vy[2], vx[0], vy[0], vx[1], vy[1]);
        e12v0 = ref_edge(vx[1], vy[1], vx[2], vy[2], vx[0], vy[0]);
        double P = 256.0;
        adx = P * (vy[1] - vy[2]) / (double)e12v0;
        ady = P * (vx[2] - vx[1]) / (double)e12v0;
        bdx = P * (vy[2] - vy[0]) / (double)e20v1;
        bdy = P * (vx[0] - vx[2]) / (double)e20v1;
        gdx = P * (vy[0] - vy[1]) / (double)e01v2;
        gdy = P * (vx[1] - vx[0]) / (double)e01v2;
        Ddx = adx * iw[0] + bdx * iw[1] + gdx * iw[2];
        Ddy = ady * iw[0] + bdy * iw[1] + gdy * iw[2];
    }

    void bary(int ix, int iy, double& A, double& B, double& G) const {
        int px = (ix << 8) + 128, py = (iy << 8) + 128;
        G = (double)ref_edge(vx[0], vy[0], vx[1], vy[1], px, py) / (double)e01v2;
        B = (double)ref_edge(vx[2], vy[2], vx[0], vy[0], px, py) / (double)e20v1;
        A = (double)ref_edge(vx[1], vy[1], vx[2], vy[2], px, py) / (double)e12v0;
    }

    void value_deriv(int slot, int ix, int iy,
                     double& fv, double& dx, double& dy) const {
        double A, B, G;
        bary(ix, iy, A, B, G);
        int mode = interp[slot];
        if (mode == PGL_FLAT) {
            fv = val[provoke][slot];
            dx = dy = 0.0;
        } else if (mode == PGL_NOPERSPECTIVE) {
            fv = A * val[0][slot] + B * val[1][slot] + G * val[2][slot];
            dx = adx * val[0][slot] + bdx * val[1][slot] + gdx * val[2][slot];
            dy = ady * val[0][slot] + bdy * val[1][slot] + gdy * val[2][slot];
        } else {
            double q0 = val[0][slot] * iw[0];
            double q1 = val[1][slot] * iw[1];
            double q2 = val[2][slot] * iw[2];
            double N = A * q0 + B * q1 + G * q2;
            double D = A * iw[0] + B * iw[1] + G * iw[2];
            fv = N / D;
            double Nx = adx * q0 + bdx * q1 + gdx * q2;
            double Ny = ady * q0 + bdy * q1 + gdy * q2;
            dx = (Nx * D - N * Ddx) / (D * D);
            dy = (Ny * D - N * Ddy) / (D * D);
        }
    }
};

// Upload 3 packed vertices and draw one triangle.
GLuint upload_tri(const std::vector<float>& d) {
    GLuint bo;
    glGenBuffers(1, &bo);
    glBindBuffer(GL_ARRAY_BUFFER, bo);
    glBufferData(GL_ARRAY_BUFFER, d.size() * sizeof(float), d.data(), GL_STATIC_DRAW);
    setup_packed_attribs();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return bo;
}

} // namespace

// ============================================================================
// test_gradients: unequal w, mixed interpolation, several viewports, exact
// callback values and derivatives vs independent double reference.
// ============================================================================
void test_gradients(Check& ck) {
    const int W = 80, H = 72;
    glContext ctx;
    pix_t* pixels = nullptr;
    expect(ck, init_glContext(&ctx, &pixels, W, H) == GL_TRUE, "init gradient context");
    ctx.dbg_output = GL_FALSE;

    // Several viewports (offset / non-square). NDC geometry stays inside.
    struct VP { int x, y, w, h; };
    VP vps[] = {{0, 0, W, H}, {5, 7, 52, 44}, {10, 4, 40, 60}, {2, 11, 64, 48}};

    const int n = 8;
    GLenum interp[n] = {
        PGL_SMOOTH, PGL_SMOOTH,       // 0,1 perspective UV-ish
        PGL_NOPERSPECTIVE,            // 2
        PGL_FLAT,                     // 3
        PGL_SMOOTH,                   // 4
        PGL_NOPERSPECTIVE,            // 5
        PGL_FLAT,                     // 6
        PGL_SMOOTH                    // 7
    };

    for (const VP& vp : vps) {
        glViewport(vp.x, vp.y, vp.w, vp.h);

        double ndc[3][2] = {{-0.55, -0.60}, {0.70, -0.35}, {-0.15, 0.80}};
        double ww[3] = {1.3, 2.1, 0.8};
        float flat3 = 7.0f, flat6 = -3.0f;

        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            float w = (float)ww[k];
            d[k * 12 + 0] = (float)(ndc[k][0] * w);
            d[k * 12 + 1] = (float)(ndc[k][1] * w);
            d[k * 12 + 3] = w;
            // out0..3 (a1)
            d[k * 12 + 4] = 0.2f + 0.35f * k;
            d[k * 12 + 5] = 0.1f + 0.42f * k;
            d[k * 12 + 6] = 1.0f - 3.0f * k;     // nopersp
            d[k * 12 + 7] = flat3;              // flat (same provoking value)
            // out4..7 (a2)
            d[k * 12 + 8]  = 3.0f + 2.5f * k;   // smooth
            d[k * 12 + 9]  = -2.0f + 6.0f * k;  // nopersp
            d[k * 12 + 10] = flat6;             // flat
            d[k * 12 + 11] = 9.0f - 1.5f * k;   // smooth
        }

        g_record_n = n;
        g_record_color = false;
        g_frags.clear();
        clear_gl_errors();
        make_record_program(n, interp);
        GLuint bo = upload_tri(d);
        GLenum err_after_draw = glGetError();

        std::string pfx = "vp " + std::to_string(vp.w) + "x" + std::to_string(vp.h) + ": ";
        expect(ck, err_after_draw == GL_NO_ERROR, pfx + "draw must not set GL error");
        expect(ck, g_frags.size() > 40, pfx + "expected many covered fragments");

        RefTri ref;
        ref.init(vp.w, vp.h, vp.x, vp.y, ndc, ww, d, n, interp, /*provoke*/ 2);

        double worst = 0.0;
        int checked = 0;
        for (const FragRec& r : g_frags) {
            expect(ck, r.valid == 1, pfx + "triangle fragment must be valid");
            for (int i = 0; i < n; ++i) {
                double fv, ex, ey;
                ref.value_deriv(i, r.ix, r.iy, fv, ex, ey);
                worst = std::max(worst, std::fabs((double)r.dx[i] - ex));
                worst = std::max(worst, std::fabs((double)r.dy[i] - ey));
                bool okv = dclose(r.in[i], fv, 3e-4, 5e-4);
                bool okx = dclose(r.dx[i], ex);
                bool oky = dclose(r.dy[i], ey);
                if (!okv || !okx || !oky) {
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                                  "%sslot %d at (%d,%d) mismatch: in %.6f/%.6f dx %.6f/%.6f dy %.6f/%.6f",
                                  pfx.c_str(), i, r.ix, r.iy, r.in[i], fv,
                                  r.dx[i], ex, r.dy[i], ey);
                    expect(ck, false, buf);
                }
                ++checked;
            }
            // Unused slots must be exactly zero.
            for (int i = n; i < kMaxC; ++i) {
                if (r.dx[i] != 0.0f || r.dy[i] != 0.0f) {
                    expect(ck, false, pfx + "unused derivative slot not zero: " + std::to_string(i));
                }
            }
        }
        expect(ck, checked > 100, pfx + "expected to compare many slot fragments");
        std::printf("  gradients %dx%d fragments=%zu worst_abs_derr=%.3e\n",
                    vp.w, vp.h, g_frags.size(), worst);
        glDeleteBuffers(1, &bo);
    }

    // Explicit y-up / sign anchor with w == 1 and position-coordinate varyings.
    {
        glViewport(0, 0, W, H);
        double ndc[3][2] = {{-0.6, -0.6}, {0.6, -0.6}, {0.0, 0.7}};
        double ww[3] = {1, 1, 1};
        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            d[k * 12 + 0] = (float)ndc[k][0];
            d[k * 12 + 1] = (float)ndc[k][1];
            d[k * 12 + 3] = 1.0f;
            d[k * 12 + 4] = (float)ndc[k][0]; // slot0 = ndc x
            d[k * 12 + 5] = (float)ndc[k][1]; // slot1 = ndc y
        }
        GLenum im[2] = {PGL_SMOOTH, PGL_SMOOTH};
        g_record_n = 2;
        g_frags.clear();
        make_record_program(2, im);
        upload_tri(d);
        int sign_ok = 0;
        for (const FragRec& r : g_frags) {
            if (r.dx[0] > 0.01f && std::fabs(r.dy[0]) < 1e-3f &&
                r.dy[1] > 0.01f && std::fabs(r.dx[1]) < 1e-3f) {
                ++sign_ok;
            }
        }
        expect(ck, sign_ok > (int)g_frags.size() / 2,
               "y-up sign/orientation of analytic derivatives");
    }

    free_glContext(&ctx);
}

// ============================================================================
// test_clipping: clipped fan output, indexed, strip/fan, instanced, and the
// provoking vertex clipped away (flat source unchanged).
// ============================================================================
namespace {

// w == 1 linear position varyings -> derivative is one constant plane for
// every primitive, independent of triangulation / indexing / instancing.
void expect_constant_plane(Check& ck, const char* tag, int draw_kind) {
    const int W = 64, H = 64;
    glContext ctx;
    pix_t* px = nullptr;
    expect(ck, init_glContext(&ctx, &px, W, H) == GL_TRUE,
           std::string(tag) + ": init context");
    ctx.dbg_output = GL_FALSE;
    glViewport(0, 0, W, H);

    // Quad (-.7,-.7),(.7,-.7),(.7,.7),(-.7,.7), slot0=ndcx slot1=ndcy.
    float q[4][12] = {};
    double pos[4][2] = {{-.7, -.7}, {.7, -.7}, {.7, .7}, {-.7, .7}};
    for (int k = 0; k < 4; ++k) {
        q[k][0] = (float)pos[k][0];
        q[k][1] = (float)pos[k][1];
        q[k][3] = 1.0f;
        q[k][4] = (float)pos[k][0];
        q[k][5] = (float)pos[k][1];
    }
    GLenum im[2] = {PGL_SMOOTH, PGL_SMOOTH};
    g_record_n = 2;
    g_record_color = false;
    g_frags.clear();
    clear_gl_errors();
    make_record_program(2, im);

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
    setup_packed_attribs();

    if (draw_kind == 1) {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else if (draw_kind == 2) {
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    } else if (draw_kind == 3) {
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 4);
    } else {
        GLuint idx[6] = {0, 1, 2, 0, 2, 3};
        GLuint ebo;
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    }

    GLenum e = glGetError();
    expect(ck, e == GL_NO_ERROR, std::string(tag) + ": no GL error");
    expect(ck, g_frags.size() > 100, std::string(tag) + ": produced fragments");

    // Full 64x64 viewport mapping.
    double sx = (64.0 - 0.01) / 2.0, sy = (64.0 - 0.01) / 2.0;
    double ex0 = 1.0 / sx, ey1 = 1.0 / sy;
    int good = 0;
    for (const FragRec& r : g_frags) {
        if (!r.valid) continue;
        if (dclose(r.dx[0], ex0, 2e-3) && dclose(r.dy[0], 0.0, 2e-3) &&
            dclose(r.dx[1], 0.0, 2e-3) && dclose(r.dy[1], ey1, 2e-3)) {
            ++good;
        }
    }
    expect(ck, good == (int)g_frags.size(),
           std::string(tag) + ": every fragment carries the plane derivatives");
    std::printf("  %s fragments=%zu plane_ok=%d\n", tag, g_frags.size(), good);
    free_glContext(&ctx);
}

} // namespace

void test_clipping(Check& ck) {
    // --- Extreme +X vertex: goes through draw_triangle_clip fan -------------
    {
        const int W = 96, H = 80;
        glContext ctx;
        pix_t* pb = nullptr;
        expect(ck, init_glContext(&ctx, &pb, W, H) == GL_TRUE, "init clipping context");
        ctx.dbg_output = GL_FALSE;
        const int VPX = 4, VPY = 6, VPW = 70, VPH = 60;
        glViewport(VPX, VPY, VPW, VPH);

        double ndc[3][2] = {{-0.7, -0.7}, {-0.7, 0.7}, {60.0, 0.2}};
        double ww[3] = {1.2, 1.7, 1.0};
        const int n = 5;
        GLenum im[n] = {PGL_SMOOTH, PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_FLAT, PGL_SMOOTH};

        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            float w = (float)ww[k];
            d[k * 12 + 0] = (float)(ndc[k][0] * w);
            d[k * 12 + 1] = (float)(ndc[k][1] * w);
            d[k * 12 + 3] = w;
            d[k * 12 + 4] = 0.3f + 0.2f * k;
            d[k * 12 + 5] = 0.8f - 0.2f * k;
            d[k * 12 + 6] = -1.0f + (float)k;
            d[k * 12 + 7] = 33.0f; // provoking vertex 2 value, clipped away
            d[k * 12 + 8] = 2.0f + (float)k;
        }

        g_record_n = n;
        g_frags.clear();
        make_record_program(n, im);
        upload_tri(d);

        expect(ck, g_frags.size() > 500, "clipped fan produced many fragments");

        RefTri ref;
        ref.init(VPW, VPH, VPX, VPY, ndc, ww, d, n, im, 2);

        int all_valid = 1, flat_kept = 1, flat_der_zero = 1;
        int smooth_exact = 0, smooth_total = 0;
        double worst_smooth = 0.0, worst_np = 0.0;
        for (const FragRec& r : g_frags) {
            if (!r.valid) all_valid = 0;
            if (r.in[3] != 33.0f) flat_kept = 0;
            if (r.dx[3] != 0.0f || r.dy[3] != 0.0f) flat_der_zero = 0;

            for (int slot : {0, 1, 4}) {
                double fv, ex, ey;
                ref.value_deriv(slot, r.ix, r.iy, fv, ex, ey);
                double e = std::max(std::fabs(r.dx[slot] - ex), std::fabs(r.dy[slot] - ey));
                worst_smooth = std::max(worst_smooth, e);
                ++smooth_total;
                if (e < 2e-4) ++smooth_exact;
            }
            // Noperspective is affine on the actual post-clip triangle; compare
            // loosely to the original plane (clip-space interpolation can shift
            // its plane when w differs). Must still be finite and close in scale.
            double fv, ex, ey;
            ref.value_deriv(2, r.ix, r.iy, fv, ex, ey);
            double en = std::max(std::fabs(r.dx[2] - ex), std::fabs(r.dy[2] - ey));
            worst_np = std::max(worst_np, en);
            bool finite = std::isfinite(r.dx[2]) && std::isfinite(r.dy[2]);
            expect(ck, finite && en < 0.02, "clipped noperspective derivative finite/near plane");
        }
        expect(ck, all_valid, "every clipped fragment valid");
        expect(ck, flat_kept, "flat value still sourced from clipped provoking vertex (33)");
        expect(ck, flat_der_zero, "flat derivatives zero through clipping");
        expect(ck, smooth_exact > smooth_total * 9 / 10,
               "most clipped smooth derivatives match double quotient reference");
        std::printf("  clip fragments=%zu worst_smooth=%.3e worst_nopersp_vs_orig=%.3f exact=%d/%d\n",
                    g_frags.size(), worst_smooth, worst_np, smooth_exact, smooth_total);
        free_glContext(&ctx);
    }

    // Indexed / strip / fan / instanced all funnel through fill derivatives.
    expect_constant_plane(ck, "indexed", 4);
    expect_constant_plane(ck, "strip", 1);
    expect_constant_plane(ck, "fan", 2);
    expect_constant_plane(ck, "instanced", 3);
}

// ============================================================================
// test_sampling: solid-color mip levels, pglTexture2DVarying level selection,
// non-adjacent slots, invalid contexts and two-context interleaving.
// ============================================================================
namespace {

float g_level_rgb[7][3] = {
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0},
    {1, 0, 1}, {0, 1, 1}, {1, 1, 1}
};

GLuint make_solid_mip_chain() {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int size = 64;
    for (int lev = 0; lev < 7; ++lev) {
        int n = size * size;
        std::vector<unsigned char> px(n * 4);
        unsigned char r = (unsigned char)(g_level_rgb[lev][0] * 255);
        unsigned char gg = (unsigned char)(g_level_rgb[lev][1] * 255);
        unsigned char b = (unsigned char)(g_level_rgb[lev][2] * 255);
        for (int i = 0; i < n; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = gg;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, lev, GL_RGBA, size, size, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        size /= 2;
    }
    return tex;
}

int expected_level(const RefTri& ref, int us, int vs, int ix, int iy,
                   double* lambda_out = nullptr) {
    double fv, dUdx, dUdy, dVdx, dVdy, tmp;
    ref.value_deriv(us, ix, iy, fv, dUdx, dUdy);
    ref.value_deriv(vs, ix, iy, fv, dVdx, dVdy);
    const double TW = 64.0, TH = 64.0;
    double ux = dUdx * TW, vx = dVdx * TH;
    double uy = dUdy * TW, vy = dVdy * TH;
    double rx = std::sqrt(ux * ux + vx * vx);
    double ry = std::sqrt(uy * uy + vy * vy);
    double rho = std::max(rx, ry);
    double lambda = rho < 1e-10 ? -16.0 : std::log2(rho);
    if (lambda_out) *lambda_out = lambda;
    int level = (int)std::floor(lambda + 0.5);
    if (level < 0) level = 0;
    if (level > 6) level = 6;
    return level;
}

bool color_is_level(const float c[4], int lev, float eps = 0.04f) {
    return std::fabs(c[0] - g_level_rgb[lev][0]) < eps &&
           std::fabs(c[1] - g_level_rgb[lev][1]) < eps &&
           std::fabs(c[2] - g_level_rgb[lev][2]) < eps &&
           std::fabs(c[3] - 1.0f) < eps;
}

} // namespace

void test_sampling(Check& ck) {
    const int W = 96, H = 96;
    glContext ctx;
    pix_t* pb = nullptr;
    expect(ck, init_glContext(&ctx, &pb, W, H) == GL_TRUE, "init sampling context");
    ctx.dbg_output = GL_FALSE;
    glViewport(0, 0, W, H);

    g_tex = make_solid_mip_chain();
    expect(ck, g_tex != 0, "created mip texture");

    // ---- Direct API contract: invalid / out-of-range contexts --------------
    {
        Shader_Builtins b;
        std::memset(&b, 0, sizeof b);
        float uv[kMaxC] = {0.25f, 0.25f};

        clear_gl_errors();
        vec4 c0 = pglTexture2DVarying(g_tex, uv, &b, 0, 1);
        expect(ck, glGetError() == GL_INVALID_OPERATION, "valid=false -> GL_INVALID_OPERATION");
        expect(ck, c0.x == 0 && c0.y == 0 && c0.z == 0 && c0.w == 0,
               "valid=false -> transparent black");

        b.pgl_DerivativesValid = GL_TRUE;
        clear_gl_errors();
        vec4 c1 = pglTexture2DVarying(g_tex, uv, &b, kMaxC, 1);
        expect(ck, glGetError() == GL_INVALID_OPERATION, "u slot OOB -> GL_INVALID_OPERATION");
        expect(ck, c1.w == 0.0f, "u slot OOB -> transparent black");

        clear_gl_errors();
        vec4 c2 = pglTexture2DVarying(g_tex, uv, &b, 0, kMaxC + 3);
        expect(ck, glGetError() == GL_INVALID_OPERATION, "v slot OOB -> GL_INVALID_OPERATION");
        expect(ck, c2.w == 0.0f, "v slot OOB -> transparent black");

        clear_gl_errors();
        vec4 c3 = pglTexture2DVarying(g_tex, nullptr, &b, 0, 1);
        expect(ck, glGetError() == GL_INVALID_OPERATION, "null fs_input -> GL_INVALID_OPERATION");
        expect(ck, c3.w == 0.0f, "null fs_input -> transparent black");

        clear_gl_errors();
        vec4 c4 = pglTexture2DVarying(g_tex, uv, nullptr, 0, 1);
        expect(ck, glGetError() == GL_INVALID_OPERATION, "null builtins -> GL_INVALID_OPERATION");
        expect(ck, c4.w == 0.0f, "null builtins -> transparent black");

        // A flagged-valid context is forwarded to Grad (zero gradients -> L0).
        clear_gl_errors();
        vec4 c5 = pglTexture2DVarying(g_tex, uv, &b, 0, 1);
        expect(ck, glGetError() == GL_NO_ERROR, "flagged valid context forwards without error");
        expect(ck, color_is_level(&c5.x, 0), "zero gradients magnify to level 0 red");
    }

    // ---- Real triangle: non-adjacent U=slot1, V=slot4, constant minify -----
    {
        double ndc[3][2] = {{-0.8, -0.8}, {0.8, -0.8}, {0.0, 0.8}};
        double ww[3] = {1, 1, 1};
        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            d[k * 12 + 0] = (float)ndc[k][0];
            d[k * 12 + 1] = (float)ndc[k][1];
            d[k * 12 + 3] = 1.0f;
            d[k * 12 + 5] = (k == 0) ? 0.0f : 1.0f; // slot1 U
            d[k * 12 + 8] = (k == 1) ? 0.0f : 1.0f; // slot4 V (non-adjacent)
        }
        int n = 6;
        GLenum im[6] = {PGL_NOPERSPECTIVE, PGL_NOPERSPECTIVE, PGL_NOPERSPECTIVE,
                        PGL_NOPERSPECTIVE, PGL_NOPERSPECTIVE, PGL_NOPERSPECTIVE};
        g_uslot = 1; g_vslot = 4;
        g_record_n = n;
        g_record_color = true;
        g_frags.clear();
        clear_gl_errors();
        make_record_program(n, im);
        upload_tri(d);
        expect(ck, glGetError() == GL_NO_ERROR, "varying sampling draw no error");

        RefTri ref;
        ref.init(W, H, 0, 0, ndc, ww, d, n, im, 2);

        int matched = 0, safe = 0;
        for (const FragRec& r : g_frags) {
            double lam;
            int lev = expected_level(ref, 1, 4, r.ix, r.iy, &lam);
            // Distance of lambda to the nearest NEAREST_MIPMAP_NEAREST flip
            // threshold (k + 0.5). Only strict-check level-interior fragments.
            double margin = std::fabs(lam - (std::floor(lam + 0.5) - 0.5));
            margin = std::min(margin, std::fabs(lam - (std::floor(lam + 0.5) + 0.5)));
            if (margin > 0.08) {
                ++safe;
                if (color_is_level(r.col, lev)) ++matched;
            }
        }
        std::printf("  constant-minify safe=%d matched=%d frags=%zu\n", safe, matched, g_frags.size());
        expect(ck, safe > 50, "constant minify: many level-interior fragments");
        expect(ck, matched == safe, "pglTexture2DVarying selects independently predicted level");
    }

    // ---- Perspective unequal-w UVs: near/far select different levels -------
    {
        double ndc[3][2] = {{-0.9, -0.9}, {0.9, -0.9}, {-0.2, 0.9}};
        double ww[3] = {2.6, 0.55, 1.4};
        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            float w = (float)ww[k];
            d[k * 12 + 0] = (float)(ndc[k][0] * w);
            d[k * 12 + 1] = (float)(ndc[k][1] * w);
            d[k * 12 + 3] = w;
            d[k * 12 + 5] = (k == 0) ? 0.0f : (k == 1 ? 1.0f : 0.2f); // U slot1
            d[k * 12 + 8] = (k == 0) ? 0.0f : (k == 2 ? 1.0f : 0.1f); // V slot4
        }
        int n = 6;
        GLenum im[6] = {PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH,
                        PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH};
        g_uslot = 1; g_vslot = 4;
        g_record_n = n;
        g_record_color = true;
        g_frags.clear();
        make_record_program(n, im);
        upload_tri(d);

        RefTri ref;
        ref.init(W, H, 0, 0, ndc, ww, d, n, im, 2);

        int level_count[7] = {};
        int matched = 0, safe = 0;
        for (const FragRec& r : g_frags) {
            double lam;
            int lev = expected_level(ref, 1, 4, r.ix, r.iy, &lam);
            level_count[lev]++;
            double margin = std::fabs(lam - (std::floor(lam + 0.5) - 0.5));
            margin = std::min(margin, std::fabs(lam - (std::floor(lam + 0.5) + 0.5)));
            if (margin > 0.08) {
                ++safe;
                if (color_is_level(r.col, lev)) ++matched;
            }
        }
        int distinct = 0;
        for (int l : level_count) if (l > 5) ++distinct;
        std::printf("  perspective levels counts:");
        for (int l = 0; l < 7; ++l) if (level_count[l]) std::printf(" L%d=%d", l, level_count[l]);
        std::printf(" safe=%d matched=%d distinct=%d\n", safe, matched, distinct);
        expect(ck, safe > 30, "perspective: many level-interior fragments");
        expect(ck, matched > safe * 95 / 100,
               "perspective varying colors match independent level prediction");
        expect(ck, distinct >= 2, "perspective slope spans multiple mip levels (near vs far)");
    }

    // ---- Point / line / fullscreen are invalid derivative contexts ---------
    g_tex = make_solid_mip_chain(); // current tex name on this context
    {
        g_bad.clear();
        GLenum im[2] = {PGL_SMOOTH, PGL_SMOOTH};
        GLuint p = pglCreateProgram(pack8_vs, bad_varying_fs, 2, im, GL_FALSE);
        glUseProgram(p);

        // Point in the middle of the frame.
        float pv[12] = {};
        pv[3] = 1.0f; // position (0,0,0,1)
        GLuint bo;
        glGenBuffers(1, &bo);
        glBindBuffer(GL_ARRAY_BUFFER, bo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(pv), pv, GL_STATIC_DRAW);
        setup_packed_attribs();
        glPointSize(1.0f);
        clear_gl_errors();
        glDrawArrays(GL_POINTS, 0, 1);
        GLenum perr = glGetError();
        expect(ck, !g_bad.empty(), "point fragment ran");
        int pok = 1;
        for (const BadRec& r : g_bad)
            if (r.valid != 0 || r.nonzero != 0 || r.col[3] != 0.0f) pok = 0;
        expect(ck, pok, "point: valid=false, arrays zero, transparent black");
        expect(ck, perr == GL_INVALID_OPERATION, "point varying sample sets INVALID_OPERATION");

        // Line across the center.
        g_bad.clear();
        float lv[2][12] = {};
        lv[0][0] = -0.5f; lv[0][3] = 1.0f;
        lv[1][0] = 0.5f;  lv[1][3] = 1.0f;
        glBufferData(GL_ARRAY_BUFFER, sizeof(lv), lv, GL_STATIC_DRAW);
        setup_packed_attribs();
        clear_gl_errors();
        glDrawArrays(GL_LINES, 0, 2);
        GLenum lerr = glGetError();
        expect(ck, !g_bad.empty(), "line fragment ran");
        int lok = 1;
        for (const BadRec& r : g_bad)
            if (r.valid != 0 || r.nonzero != 0 || r.col[3] != 0.0f) lok = 0;
        expect(ck, lok, "line: valid=false, arrays zero, transparent black");
        expect(ck, lerr == GL_INVALID_OPERATION, "line varying sample sets INVALID_OPERATION");

        // Polygon non-fill modes route triangles through point/line paths.
        float tv[3][12] = {};
        double tpos[3][2] = {{-0.6, -0.5}, {0.6, -0.5}, {0.0, 0.65}};
        for (int k = 0; k < 3; ++k) {
            tv[k][0] = (float)tpos[k][0];
            tv[k][1] = (float)tpos[k][1];
            tv[k][3] = 1.0f;
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof tv, tv, GL_STATIC_DRAW);
        setup_packed_attribs();
        for (GLenum pmode : {GL_LINE, GL_POINT}) {
            glPolygonMode(GL_FRONT_AND_BACK, pmode);
            g_bad.clear();
            clear_gl_errors();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            GLenum perr2 = glGetError();
            char msg[96];
            std::snprintf(msg, sizeof msg, "polygon mode 0x%x produced fragments", (unsigned)pmode);
            expect(ck, !g_bad.empty(), msg);
            int pok2 = 1;
            for (const BadRec& r : g_bad)
                if (r.valid != 0 || r.nonzero != 0 || r.col[3] != 0.0f) pok2 = 0;
            std::snprintf(msg, sizeof msg, "polygon mode 0x%x invalid derivative context", (unsigned)pmode);
            expect(ck, pok2, msg);
            std::snprintf(msg, sizeof msg, "polygon mode 0x%x sets INVALID_OPERATION", (unsigned)pmode);
            expect(ck, perr2 == GL_INVALID_OPERATION, msg);
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Fullscreen helper passes NULL fs_input and no triangle context.
        g_bad.clear();
        clear_gl_errors();
        pglDrawFrame2(bad_varying_fs, nullptr);
        GLenum ferr = glGetError();
        expect(ck, (int)g_bad.size() == W * H, "fullscreen program covers all pixels");
        int fok = 1;
        for (const BadRec& r : g_bad)
            if (r.valid != 0 || r.nonzero != 0 || r.col[3] != 0.0f) fok = 0;
        expect(ck, fok, "fullscreen: valid=false, arrays zero, transparent black");
        expect(ck, ferr == GL_INVALID_OPERATION, "fullscreen varying sample sets INVALID_OPERATION");
    }

    free_glContext(&ctx);

    // ---- Two contexts interleaved: independent error + derivative state -----
    {
        glContext A, B;
        pix_t* pa = nullptr; pix_t* pb2 = nullptr;
        expect(ck, init_glContext(&A, &pa, 40, 40) == GL_TRUE, "init context A");
        A.dbg_output = GL_FALSE;
        GLuint texA = make_solid_mip_chain();

        expect(ck, init_glContext(&B, &pb2, 32, 32) == GL_TRUE, "init context B");
        B.dbg_output = GL_FALSE;
        GLuint texB = make_solid_mip_chain();

        Shader_Builtins zb;
        std::memset(&zb, 0, sizeof zb);
        float uv[4] = {0, 0, 0, 0};

        set_glContext(&A);
        pglTexture2DVarying(texA, uv, &zb, 0, 1); // sets A.error
        set_glContext(&B);
        pglTexture2DVarying(texB, uv, &zb, 0, 1); // sets B.error

        expect(ck, A.error == GL_INVALID_OPERATION, "context A stored its own error");
        expect(ck, B.error == GL_INVALID_OPERATION, "context B stored its own error");

        // Triangle on A is valid even though B was used in between.
        set_glContext(&A);
        double ndc[3][2] = {{-.6, -.6}, {.6, -.6}, {0, .7}};
        double ww[3] = {1, 1, 1};
        std::vector<float> d(3 * 12, 0.0f);
        for (int k = 0; k < 3; ++k) {
            d[k * 12] = (float)ndc[k][0]; d[k * 12 + 1] = (float)ndc[k][1]; d[k * 12 + 3] = 1;
            d[k * 12 + 4] = (float)ndc[k][0]; d[k * 12 + 5] = (float)ndc[k][1];
        }
        GLenum im[2] = {PGL_SMOOTH, PGL_SMOOTH};
        g_record_n = 2; g_record_color = false; g_frags.clear();
        make_record_program(2, im);
        upload_tri(d);
        int avalid = 1;
        for (const FragRec& r : g_frags) if (!r.valid) avalid = 0;
        expect(ck, !g_frags.empty() && avalid, "A triangle valid after interleaving B");
        expect(ck, glGetError() == GL_INVALID_OPERATION, "reading A error returns its stored error");
        expect(ck, glGetError() == GL_NO_ERROR, "A error now clear");

        // Point on B is invalid (no leak from A's triangle stack storage).
        set_glContext(&B);
        g_bad.clear();
        GLenum im2[2] = {PGL_SMOOTH, PGL_SMOOTH};
        GLuint p = pglCreateProgram(pack8_vs, bad_varying_fs, 2, im2, GL_FALSE);
        glUseProgram(p);
        float pv[12] = {}; pv[3] = 1.0f;
        GLuint bo;
        glGenBuffers(1, &bo);
        glBindBuffer(GL_ARRAY_BUFFER, bo);
        glBufferData(GL_ARRAY_BUFFER, sizeof pv, pv, GL_STATIC_DRAW);
        setup_packed_attribs();
        glDrawArrays(GL_POINTS, 0, 1);
        int bok = !g_bad.empty();
        for (const BadRec& r : g_bad) if (r.valid != 0 || r.nonzero != 0) bok = 0;
        expect(ck, bok, "B point stays invalid after A triangle");
        expect(ck, glGetError() == GL_INVALID_OPERATION, "B still has its own stored error");

        free_glContext(&A);
        free_glContext(&B);
    }
}
