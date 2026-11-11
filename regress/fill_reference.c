/*
 * regress/fill_reference.c
 *
 * Regression tests for PlutoVG solid fills, fill rules and clipping.
 *
 * The product is exercised only through its public canvas/path/surface API.
 * Every expected coverage value comes from the independent analytic
 * reference in ref_geometry.c (horizontal-ray crossings, directed winding /
 * parity and pixel-area integration) - never from the product's own
 * flattening, rasterization, containment or extents entry points.
 *
 * Coverage is read back as opaque-white alpha on a transparent surface.
 * Clipping follows the product's documented 8-bit convention
 * (a*b/255 truncated); the reference propagates its lower/upper coverage
 * intervals through the same monotone byte operation instead of treating an
 * ideal-area product as an exact byte.
 */
#include "plutovg.h"
#include "ref_geometry.h"
#include "ppm_image.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_CLIP_LAYERS 3
#define CANVAS 16

typedef struct {
    const char* name;
    int w, h;
    int rule;
    int curves;
    int clip_layers;
    int full_canvas_fill;
    rshape_t fill;
    rshape_t clip[MAX_CLIP_LAYERS];
} scene_geom_t;

typedef struct {
    long asserts;
    int scenes_run;
    int failures;
    char first_fail[768];
    const char* outdir;
    double max_band;
    double worst_line_stability;
    double worst_curve_stability;
} report_t;

static report_t g_rep;

static void record_fail(const char* scene, const char* fmt, ...)
{
    g_rep.failures++;
    if(g_rep.first_fail[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    snprintf(g_rep.first_fail, sizeof(g_rep.first_fail),
        "scene=%s ", scene);
    size_t n = strlen(g_rep.first_fail);
    vsnprintf(g_rep.first_fail + n,
        sizeof(g_rep.first_fail) - n, fmt, ap);
    va_end(ap);
}

#define CHECK(scene, cond, ...)                                              \
    do {                                                                     \
        g_rep.asserts++;                                                     \
        if(!(cond)) record_fail((scene), __VA_ARGS__);                       \
    } while(0)

static uint32_t pixel_argb(const plutovg_surface_t* s, int x, int y)
{
    const uint32_t* row = (const uint32_t*)(
        plutovg_surface_get_data((plutovg_surface_t*)s) +
        (size_t)y * (size_t)plutovg_surface_get_stride((plutovg_surface_t*)s));
    return row[x];
}

static int pixel_alpha(const plutovg_surface_t* s, int x, int y)
{
    return (int)(pixel_argb(s, x, y) >> 24);
}

/* Independent replication of the product's premult byte rounding. */
static int byte_mul(int x, int a)
{
    int t = x * a;
    return (t + (t >> 8) + 128) >> 8;
}

static int byte_combine(int a, int b)
{
    return (a * b) / 255;
}

/* ------------------------------------------------------------------ */
/* Reference coverage intervals                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int lo, exact, hi;
} byte_interval_t;

static int clamp255(double v)
{
    long b = lround(v * 255.0);
    if(b < 0) b = 0;
    if(b > 255) b = 255;
    return (int)b;
}

static void exact_grid(const rshape_t* s, int W, int H, int subrows,
    double* out)
{
    rcov_t* g = (rcov_t*)calloc((size_t)(W * H), sizeof(rcov_t));
    rshape_coverage(s, W, H, subrows, 0.0, g);
    for(int i = 0; i < W * H; i++)
        out[i] = g[i].exact;
    free(g);
}

static int shape_has_curves(const rshape_t* s)
{
    for(int c = 0; c < s->size; c++)
        for(int j = 0; j < s->contours[c].size; j++)
            if(s->contours[c].data[j].type == RSEG_CUBIC)
                return 1;
    return 0;
}

static void fill_interval(const rshape_t* s, int W, int H, int curves,
    byte_interval_t* out, double* band_used)
{
    if(!curves) {
        /* Two integration densities prove the row integration converged. */
        double* g1 = (double*)calloc((size_t)(W * H), sizeof(double));
        double* g2 = (double*)calloc((size_t)(W * H), sizeof(double));
        exact_grid(s, W, H, 32, g1);
        exact_grid(s, W, H, 96, g2);
        double worst = 0;
        for(int i = 0; i < W * H; i++) {
            double d = fabs(g1[i] - g2[i]);
            if(d > worst) worst = d;
            out[i].lo = out[i].exact = out[i].hi = clamp255(g2[i]);
        }
        if(worst > g_rep.worst_line_stability)
            g_rep.worst_line_stability = worst;
        if(band_used) *band_used = 0.0;
        free(g1);
        free(g2);
        return;
    }

    /* Geometric band derived forward from the product's own stop rule,
       subdivision depth and 26.6 fixed-point rounding. */
    double geom = rshape_curve_band(s);
    if(geom > g_rep.max_band) g_rep.max_band = geom;
    if(geom > 0.25) {
        fprintf(stderr, "curve band %.5f exceeds 0.25 px\n", geom);
        abort();
    }

    /* Ideal area at two analytic integration densities; the row measure is
       exact per probe row, so convergence here validates the integrator
       independently of the geometric band. */
    double* e1 = (double*)calloc((size_t)(W * H), sizeof(double));
    double* e2 = (double*)calloc((size_t)(W * H), sizeof(double));
    exact_grid(s, W, H, 32, e1);
    exact_grid(s, W, H, 96, e2);
    double worst_stab = 0;
    for(int i = 0; i < W * H; i++) {
        double d = fabs(e1[i] - e2[i]);
        if(d > worst_stab) worst_stab = d;
    }
    if(worst_stab > 2.0 / 255.0) {
        fprintf(stderr, "reference unstable: %.5f > 2/255\n", worst_stab);
        abort();
    }
    if(worst_stab > g_rep.worst_curve_stability)
        g_rep.worst_curve_stability = worst_stab;

    /* Eroded/dilated coverage bounds.  The sample lattice half-cell h is
       added to the geometric band so both bounds stay conservative. */
    rcov_t* gb = (rcov_t*)calloc((size_t)(W * H), sizeof(rcov_t));
    double disc;
    rshape_coverage_banded(s, W, H, 16, geom, gb, &disc);
    for(int i = 0; i < W * H; i++) {
        out[i].lo = clamp255(gb[i].lo);
        out[i].exact = clamp255(e2[i]);
        out[i].hi = clamp255(gb[i].hi);
    }
    if(band_used) *band_used = geom;
    free(e1);
    free(e2);
    free(gb);
}

static byte_interval_t combine_interval(byte_interval_t a,
    byte_interval_t b)
{
    byte_interval_t r;
    r.lo = byte_combine(a.lo, b.lo);
    r.hi = byte_combine(a.hi, b.hi);
    r.exact = byte_combine(a.exact, b.exact);
    if(r.lo > 255) r.lo = 255;
    if(r.hi > 255) r.hi = 255;
    return r;
}

/* ------------------------------------------------------------------ */
/* Reference geometry builders and product mirrors                     */
/* ------------------------------------------------------------------ */

static rcontour_t* ref_begin(rshape_t* s)
{
    return rshape_begin_contour(s);
}

static void ref_rect(rshape_t* s, double x, double y, double w, double h,
    int cw)
{
    rcontour_t* c = ref_begin(s);
    rcontour_move_to(c, x, y);
    if(cw) {
        rcontour_line_to(c, x + w, y);
        rcontour_line_to(c, x + w, y + h);
        rcontour_line_to(c, x, y + h);
    } else {
        rcontour_line_to(c, x, y + h);
        rcontour_line_to(c, x + w, y + h);
        rcontour_line_to(c, x + w, y);
    }
    rcontour_close(c);
}

static void canvas_rect_dir(plutovg_canvas_t* cv, double x, double y,
    double w, double h, int cw)
{
    if(cw) {
        plutovg_canvas_rect(cv, x, y, w, h);
        return;
    }
    plutovg_canvas_move_to(cv, x, y);
    plutovg_canvas_line_to(cv, x, y + h);
    plutovg_canvas_line_to(cv, x + w, y + h);
    plutovg_canvas_line_to(cv, x + w, y);
    plutovg_canvas_close_path(cv);
}

static void build_arch_shape(rshape_t* s)
{
    rcontour_t* c = ref_begin(s);
    rcontour_move_to(c, 2, 13);
    rcontour_line_to(c, 2, 9);
    rcontour_cubic_to(c, 2, 3, 14, 3, 14, 9);
    rcontour_line_to(c, 14, 13);
    rcontour_close(c);
}

static void draw_arch_path(plutovg_canvas_t* cv)
{
    plutovg_canvas_move_to(cv, 2, 13);
    plutovg_canvas_line_to(cv, 2, 9);
    plutovg_canvas_cubic_to(cv, 2, 3, 14, 3, 14, 9);
    plutovg_canvas_line_to(cv, 14, 13);
    plutovg_canvas_close_path(cv);
}

/* ------------------------------------------------------------------ */
/* White-coverage scene comparison                                     */
/* ------------------------------------------------------------------ */

static void write_scene_image(const char* name, int W, int H,
    const plutovg_surface_t* surf, const byte_interval_t* ref)
{
    uint8_t* refb = (uint8_t*)malloc((size_t)(W * H));
    uint8_t* gotb = (uint8_t*)malloc((size_t)(W * H));
    for(int i = 0; i < W * H; i++) {
        refb[i] = (uint8_t)ref[i].exact;
        gotb[i] = (uint8_t)pixel_alpha(surf, i % W, i / W);
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", g_rep.outdir, name);
#ifdef _WIN32
    mkdir(g_rep.outdir);
#else
    mkdir(g_rep.outdir, 0755);
#endif
    ppm_write_triptych(path, refb, gotb, W, H);
    free(refb);
    free(gotb);
}

static void reference_final(const scene_geom_t* sc, byte_interval_t* final)
{
    int W = sc->w, H = sc->h, N = W * H;
    if(sc->clip_layers == 0) {
        double band;
        fill_interval(&sc->fill, W, H, sc->curves, final, &band);
        CHECK(sc->name, band <= 0.25 + 1e-9,
            "band %.4f exceeds quarter pixel", band);
        return;
    }

    for(int i = 0; i < N; i++)
        final[i].lo = final[i].exact = final[i].hi = 255;
    for(int k = 0; k < sc->clip_layers; k++) {
        byte_interval_t* layer =
            (byte_interval_t*)calloc((size_t)N, sizeof(byte_interval_t));
        double band;
        fill_interval(&sc->clip[k], W, H,
            shape_has_curves(&sc->clip[k]), layer, &band);
        CHECK(sc->name, band <= 0.25 + 1e-9,
            "clip[%d] band %.4f exceeds quarter pixel", k, band);
        for(int i = 0; i < N; i++)
            final[i] = combine_interval(final[i], layer[i]);
        free(layer);
    }
    if(!sc->full_canvas_fill) {
        byte_interval_t* fill =
            (byte_interval_t*)calloc((size_t)N, sizeof(byte_interval_t));
        double band;
        fill_interval(&sc->fill, W, H, sc->curves, fill, &band);
        for(int i = 0; i < N; i++)
            final[i] = combine_interval(final[i], fill[i]);
        free(fill);
    }
}

static void compare_white(const scene_geom_t* sc,
    const plutovg_surface_t* surf)
{
    int W = sc->w, H = sc->h, N = W * H;
    byte_interval_t* final =
        (byte_interval_t*)calloc((size_t)N, sizeof(byte_interval_t));
    reference_final(sc, final);

    int max_diff = -1, max_x = 0, max_y = 0;
    double max_rel = 0;
    int partial = 0, full = 0, empty = 0;
    int tight_lo = 0, tight_hi = 0, max_width = 0;
    for(int y = 0; y < H; y++) {
        for(int x = 0; x < W; x++) {
            int i = y * W + x;
            int width = final[i].hi - final[i].lo;
            if(width > max_width) max_width = width;
            if(final[i].lo > 0) tight_lo++;
            if(final[i].hi < 255) tight_hi++;
            int got = pixel_alpha(surf, x, y);
            int lo = final[i].lo - 2;
            int hi = final[i].hi + 2;
            if(lo < 0) lo = 0;
            if(hi > 255) hi = 255;
            CHECK(sc->name, got >= lo && got <= hi,
                "pixel=(%d,%d) expected coverage in [%d,%d] ideal=%d got=%d",
                x, y, lo, hi, final[i].exact, got);
            int d = abs(got - final[i].exact);
            if(d > max_diff) {
                max_diff = d;
                max_x = x;
                max_y = y;
                double denom = final[i].exact ? (double)final[i].exact
                                              : 255.0;
                max_rel = (double)d / denom;
            }
            if(got == 255) full++;
            else if(got == 0) empty++;
            else partial++;
        }
    }
    CHECK(sc->name, full > 0, "no fully covered pixels");
    CHECK(sc->name, empty > 0, "no fully empty pixels");
    CHECK(sc->name, partial > 0, "no partially covered pixels");
    if(sc->curves || sc->clip_layers > 0) {
        /* the geometric band must remain a real constraint, never an
           unrestricted 0..255 envelope */
        CHECK(sc->name, max_width < 220,
            "coverage interval too loose width=%d", max_width);
        CHECK(sc->name, tight_lo > 0 && tight_hi > 0,
            "band bounds never bind lo=%d hi=%d", tight_lo, tight_hi);
    }

    printf("FILL_SCENE %-28s pixels=%d max|got-ideal|=%d at=(%d,%d) "
           "rel=%.4f classes(f/p/e)=%d/%d/%d bandwidth<=%d\n",
        sc->name, N, max_diff, max_x, max_y, max_rel, full, partial, empty,
        max_width);
    write_scene_image(sc->name, W, H, surf, final);
    free(final);
}

/* ------------------------------------------------------------------ */
/* Scene table                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;
    int w, h, rule, curves;
    void (*build)(scene_geom_t*);
    void (*draw)(plutovg_canvas_t*);
} white_scene_t;

static void set_base(scene_geom_t* sc, const white_scene_t* d)
{
    sc->name = d->name;
    sc->w = d->w;
    sc->h = d->h;
    sc->rule = d->rule;
    sc->curves = d->curves;
    rshape_init(&sc->fill, d->rule);
    for(int i = 0; i < MAX_CLIP_LAYERS; i++)
        rshape_init(&sc->clip[i], d->rule);
    d->build(sc);
}

/* --- nested / duplicate / open ------------------------------------ */

static void b_same_eo(scene_geom_t* sc)
{
    ref_rect(&sc->fill, 2.5, 2.5, 11.0, 11.0, 1);
    ref_rect(&sc->fill, 5.5, 5.5, 5.0, 5.0, 1);
}
static void d_same_eo(plutovg_canvas_t* cv)
{
    canvas_rect_dir(cv, 2.5, 2.5, 11.0, 11.0, 1);
    canvas_rect_dir(cv, 5.5, 5.5, 5.0, 5.0, 1);
    plutovg_canvas_fill(cv);
}

static void b_same_nz(scene_geom_t* sc) { b_same_eo(sc); }
static void d_same_nz(plutovg_canvas_t* cv) { d_same_eo(cv); }

static void b_opp_eo(scene_geom_t* sc)
{
    ref_rect(&sc->fill, 2.5, 2.5, 11.0, 11.0, 1);
    ref_rect(&sc->fill, 5.5, 5.5, 5.0, 5.0, 0);
}
static void d_opp_eo(plutovg_canvas_t* cv)
{
    canvas_rect_dir(cv, 2.5, 2.5, 11.0, 11.0, 1);
    canvas_rect_dir(cv, 5.5, 5.5, 5.0, 5.0, 0);
    plutovg_canvas_fill(cv);
}
static void b_opp_nz(scene_geom_t* sc) { b_opp_eo(sc); }
static void d_opp_nz(plutovg_canvas_t* cv) { d_opp_eo(cv); }

static void b_dup(scene_geom_t* sc)
{
    /* Integer-aligned identical pair: with no partial edge cells, even-odd
       cancels the doubled coverage exactly while non-zero stays solid. */
    ref_rect(&sc->fill, 2, 2, 6, 6, 1);
    ref_rect(&sc->fill, 2, 2, 6, 6, 1);
    /* Unrelated fractional rect supplies the partial-coverage class. */
    ref_rect(&sc->fill, 10.5, 10.5, 3.5, 3.5, 1);
}
static void d_dup(plutovg_canvas_t* cv)
{
    canvas_rect_dir(cv, 2, 2, 6, 6, 1);
    canvas_rect_dir(cv, 2, 2, 6, 6, 1);
    canvas_rect_dir(cv, 10.5, 10.5, 3.5, 3.5, 1);
    plutovg_canvas_fill(cv);
}

static void b_open(scene_geom_t* sc)
{
    rcontour_t* c = ref_begin(&sc->fill);
    rcontour_move_to(c, 3.5, 13.5);
    rcontour_line_to(c, 13.5, 13.5);
    rcontour_line_to(c, 13.5, 3.5); /* no explicit close */
}
static void d_open(plutovg_canvas_t* cv)
{
    plutovg_canvas_move_to(cv, 3.5, 13.5);
    plutovg_canvas_line_to(cv, 13.5, 13.5);
    plutovg_canvas_line_to(cv, 13.5, 3.5);
    plutovg_canvas_fill(cv);
}

/* --- curves -------------------------------------------------------- */

static void b_arch(scene_geom_t* sc) { build_arch_shape(&sc->fill); }
static void d_arch(plutovg_canvas_t* cv)
{
    draw_arch_path(cv);
    plutovg_canvas_fill(cv);
}

static void b_scurve(scene_geom_t* sc)
{
    rcontour_t* c = ref_begin(&sc->fill);
    rcontour_move_to(c, 2, 12);
    rcontour_cubic_to(c, 6, 2, 10, 14, 14, 4);
    rcontour_line_to(c, 14, 14);
    rcontour_line_to(c, 2, 14);
    rcontour_close(c);
}
static void d_scurve(plutovg_canvas_t* cv)
{
    plutovg_canvas_move_to(cv, 2, 12);
    plutovg_canvas_cubic_to(cv, 6, 2, 10, 14, 14, 4);
    plutovg_canvas_line_to(cv, 14, 14);
    plutovg_canvas_line_to(cv, 2, 14);
    plutovg_canvas_close_path(cv);
    plutovg_canvas_fill(cv);
}

static void b_tangent(scene_geom_t* sc)
{
    rcontour_t* c = ref_begin(&sc->fill);
    rcontour_move_to(c, 2, 10);
    rcontour_cubic_to(c, 4, 2, 12, 2, 14, 10);
    rcontour_line_to(c, 14, 13);
    rcontour_line_to(c, 2, 13);
    rcontour_close(c);
}
static void d_tangent(plutovg_canvas_t* cv)
{
    plutovg_canvas_move_to(cv, 2, 10);
    plutovg_canvas_cubic_to(cv, 4, 2, 12, 2, 14, 10);
    plutovg_canvas_line_to(cv, 14, 13);
    plutovg_canvas_line_to(cv, 2, 13);
    plutovg_canvas_close_path(cv);
    plutovg_canvas_fill(cv);
}

static void build_lens(rshape_t* s, double x0)
{
    rcontour_t* c = ref_begin(s);
    rcontour_move_to(c, x0, 8);
    rcontour_cubic_to(c, x0, 2, x0 + 10, 2, x0 + 10, 8);
    rcontour_cubic_to(c, x0 + 10, 14, x0, 14, x0, 8);
    rcontour_close(c);
}
static void draw_lens(plutovg_canvas_t* cv, double x0)
{
    plutovg_canvas_move_to(cv, x0, 8);
    plutovg_canvas_cubic_to(cv, x0, 2, x0 + 10, 2, x0 + 10, 8);
    plutovg_canvas_cubic_to(cv, x0 + 10, 14, x0, 14, x0, 8);
    plutovg_canvas_close_path(cv);
}
static void b_lenses(scene_geom_t* sc)
{
    build_lens(&sc->fill, 1);
    build_lens(&sc->fill, 5);
}
static void d_lenses(plutovg_canvas_t* cv)
{
    draw_lens(cv, 1);
    draw_lens(cv, 5);
    plutovg_canvas_fill(cv);
}

/* --- transforms (reference maps the control points itself) --------- */

static void b_xform_shear(scene_geom_t* sc)
{
    build_arch_shape(&sc->fill);
    rshape_set_matrix(&sc->fill, 1.0, 0.0, 0.5, 1.0, 0.5, 0.25);
}
static void d_xform_shear(plutovg_canvas_t* cv)
{
    plutovg_matrix_t m = PLUTOVG_MAKE_MATRIX(1, 0, 0.5f, 1, 0.5f, 0.25f);
    plutovg_canvas_set_matrix(cv, &m);
    draw_arch_path(cv);
    plutovg_canvas_fill(cv);
}

static void b_xform_reflect(scene_geom_t* sc)
{
    build_arch_shape(&sc->fill);
    rshape_set_matrix(&sc->fill, -1.5, 0.0, 0.0, 0.75, 15.5, 1.25);
}
static void d_xform_reflect(plutovg_canvas_t* cv)
{
    plutovg_matrix_t m = PLUTOVG_MAKE_MATRIX(-1.5f, 0, 0, 0.75f, 15.5f, 1.25f);
    plutovg_canvas_set_matrix(cv, &m);
    draw_arch_path(cv);
    plutovg_canvas_fill(cv);
}

static void b_xform_border(scene_geom_t* sc)
{
    build_arch_shape(&sc->fill);
    rshape_set_matrix(&sc->fill, 1.25, 0.0, 0.0, 1.0, -6.0, 0.0);
}
static void d_xform_border(plutovg_canvas_t* cv)
{
    plutovg_matrix_t m = PLUTOVG_MAKE_MATRIX(1.25f, 0, 0, 1, -6, 0);
    plutovg_canvas_set_matrix(cv, &m);
    draw_arch_path(cv);
    plutovg_canvas_fill(cv);
}

/* --- clip scenes --------------------------------------------------- */

static void b_clip_rects(scene_geom_t* sc)
{
    sc->clip_layers = 2;
    sc->full_canvas_fill = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    rshape_init(&sc->clip[1], RRULE_NON_ZERO);
    ref_rect(&sc->clip[0], 1.5, 1.5, 9.5, 13.0, 1);
    ref_rect(&sc->clip[1], 5.5, 3.5, 9.5, 9.5, 1);
}
static void d_clip_rects(plutovg_canvas_t* cv)
{
    plutovg_canvas_clip_rect(cv, 1.5, 1.5, 9.5, 13.0);
    plutovg_canvas_clip_rect(cv, 5.5, 3.5, 9.5, 9.5);
    plutovg_canvas_paint(cv);
}

static void b_clip_freeze(scene_geom_t* sc)
{
    sc->clip_layers = 2;
    sc->full_canvas_fill = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    rshape_init(&sc->clip[1], RRULE_NON_ZERO);
    rshape_set_matrix(&sc->clip[0], 1.5, 0, 0, 1.25, 0.5, 0.25);
    rshape_set_matrix(&sc->clip[1], 1.0, 0.25, 0, 1.0, 1.0, 2.0);
    ref_rect(&sc->clip[0], 1, 1, 6, 8, 1);
    ref_rect(&sc->clip[1], 2, 2, 8, 6, 1);
}
static void d_clip_freeze(plutovg_canvas_t* cv)
{
    plutovg_matrix_t A = PLUTOVG_MAKE_MATRIX(1.5f, 0, 0, 1.25f, 0.5f, 0.25f);
    plutovg_matrix_t B = PLUTOVG_MAKE_MATRIX(1, 0.25f, 0, 1, 1, 2);
    plutovg_canvas_set_matrix(cv, &A);
    plutovg_canvas_clip_rect(cv, 1, 1, 6, 8);
    plutovg_canvas_set_matrix(cv, &B);
    plutovg_canvas_clip_rect(cv, 2, 2, 8, 6);
    plutovg_canvas_reset_matrix(cv);
    plutovg_canvas_paint(cv);
}

static void b_clip_nested(scene_geom_t* sc)
{
    sc->clip_layers = 2;
    sc->full_canvas_fill = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    rshape_init(&sc->clip[1], RRULE_NON_ZERO);
    ref_rect(&sc->clip[0], 1.5, 1.5, 13.0, 13.0, 1);
    ref_rect(&sc->clip[1], 4.5, 4.5, 8.0, 8.0, 1);
}
static void d_clip_nested(plutovg_canvas_t* cv)
{
    plutovg_canvas_clip_rect(cv, 1.5, 1.5, 13.0, 13.0);
    plutovg_canvas_save(cv);
    plutovg_canvas_clip_rect(cv, 4.5, 4.5, 8.0, 8.0);
    plutovg_canvas_paint(cv); /* intersection active inside the save */
    plutovg_canvas_restore(cv);
}

/* After restore the inner clip must be gone: a fresh paint sees only the
   outer clip, even when the released state block is recycled by a second
   save that changes the fill rule. */
static void b_clip_reuse(scene_geom_t* sc)
{
    sc->clip_layers = 1;
    sc->full_canvas_fill = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    ref_rect(&sc->clip[0], 1.5, 1.5, 13.0, 13.0, 1);
}
static void d_clip_reuse(plutovg_canvas_t* cv)
{
    plutovg_canvas_clip_rect(cv, 1.5, 1.5, 13.0, 13.0);
    plutovg_canvas_save(cv);
    plutovg_canvas_clip_rect(cv, 4.5, 4.5, 3.0, 3.0);
    plutovg_canvas_restore(cv); /* inner clip released to freed_state */
    plutovg_canvas_save(cv);    /* recycles that released state block */
    plutovg_canvas_set_fill_rule(cv, PLUTOVG_FILL_RULE_EVEN_ODD);
    plutovg_canvas_restore(cv);
    plutovg_canvas_paint(cv);   /* outer clip and NZ rule only */
}

static void b_clip_outer_only(scene_geom_t* sc)
{
    sc->clip_layers = 1;
    sc->full_canvas_fill = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    ref_rect(&sc->clip[0], 1.5, 1.5, 13.0, 13.0, 1);
}
static void d_clip_outer_only(plutovg_canvas_t* cv)
{
    plutovg_canvas_clip_rect(cv, 1.5, 1.5, 13.0, 13.0);
    plutovg_canvas_save(cv);
    plutovg_canvas_clip_rect(cv, 4.5, 4.5, 8.0, 8.0);
    plutovg_canvas_restore(cv);
    plutovg_canvas_paint(cv); /* inner clip dropped by restore */
}

static void b_clip_curve(scene_geom_t* sc)
{
    sc->clip_layers = 2;
    sc->full_canvas_fill = 1;
    sc->curves = 1;
    rshape_init(&sc->clip[0], RRULE_NON_ZERO);
    rshape_init(&sc->clip[1], RRULE_NON_ZERO);
    ref_rect(&sc->clip[0], 0.5, 1.5, 15.0, 12.5, 1);
    rcontour_t* c = rshape_begin_contour(&sc->clip[1]);
    rcontour_move_to(c, 2, 13);
    rcontour_line_to(c, 2, 9);
    rcontour_cubic_to(c, 2, 3, 14, 3, 14, 9);
    rcontour_line_to(c, 14, 13);
    rcontour_close(c);
}
static void d_clip_curve(plutovg_canvas_t* cv)
{
    plutovg_canvas_clip_rect(cv, 0.5, 1.5, 15.0, 12.5);
    draw_arch_path(cv);
    plutovg_canvas_clip(cv);
    plutovg_canvas_paint(cv);
}

static const white_scene_t g_white[] = {
    { "line_same_dir_evenodd", CANVAS, CANVAS, RRULE_EVEN_ODD, 0,
        b_same_eo, d_same_eo },
    { "line_same_dir_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_same_nz, d_same_nz },
    { "line_opp_dir_evenodd", CANVAS, CANVAS, RRULE_EVEN_ODD, 0,
        b_opp_eo, d_opp_eo },
    { "line_opp_dir_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_opp_nz, d_opp_nz },
    { "line_duplicate_evenodd", CANVAS, CANVAS, RRULE_EVEN_ODD, 0,
        b_dup, d_dup },
    { "line_duplicate_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_dup, d_dup },
    { "line_open_path_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_open, d_open },
    { "cubic_arch_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_arch, d_arch },
    { "cubic_s_shape_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_scurve, d_scurve },
    { "cubic_tangent_evenodd", CANVAS, CANVAS, RRULE_EVEN_ODD, 1,
        b_tangent, d_tangent },
    { "cubic_overlap_nonzero", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_lenses, d_lenses },
    { "cubic_overlap_evenodd", CANVAS, CANVAS, RRULE_EVEN_ODD, 1,
        b_lenses, d_lenses },
    { "xform_shear_arch", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_xform_shear, d_xform_shear },
    { "xform_reflect_scale_arch", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_xform_reflect, d_xform_reflect },
    { "xform_cross_border_arch", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_xform_border, d_xform_border },
    { "clip_two_rects", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_clip_rects, d_clip_rects },
    { "clip_matrix_freeze", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_clip_freeze, d_clip_freeze },
    { "clip_nested_save_restore", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_clip_nested, d_clip_nested },
    { "clip_outer_after_restore", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_clip_outer_only, d_clip_outer_only },
    { "clip_state_reuse", CANVAS, CANVAS, RRULE_NON_ZERO, 0,
        b_clip_reuse, d_clip_reuse },
    { "clip_curve_intersect", CANVAS, CANVAS, RRULE_NON_ZERO, 1,
        b_clip_curve, d_clip_curve },
};
static const int g_white_count = (int)(sizeof(g_white) / sizeof(g_white[0]));

/* ------------------------------------------------------------------ */
/* Analytic ray bookkeeping probes                                     */
/* ------------------------------------------------------------------ */

static void probe_ray_invariants(const scene_geom_t* sc)
{
    /* S-curve: a middle scanline crosses the cubic three times; the ray
       must still return to winding/parity zero at both far ends. */
    if(strcmp(sc->name, "cubic_s_shape_nonzero") == 0) {
        int wl, pl, wr, pr;
        rshape_ray_state(&sc->fill, 8.0, 0.25, &wl, &pl);
        rshape_ray_state(&sc->fill, 8.0, 15.75, &wr, &pr);
        CHECK(sc->name, wl == 0 && pl == 0,
            "s-curve left ray not empty w=%d p=%d", wl, pl);
        CHECK(sc->name, wr == 0 && pr == 0,
            "s-curve right ray not empty w=%d p=%d", wr, pr);
        int toggles = 0, prev = 0;
        for(int x = 1; x < 16; x++) {
            int w, p;
            rshape_ray_state(&sc->fill, 8.0, x + 0.5, &w, &p);
            if(p != prev) toggles++;
            prev = p;
        }
        CHECK(sc->name, (toggles & 1) == 0 && toggles >= 2,
            "s-curve parity toggles=%d not even/multi-root", toggles);
    }

    /* Horizontal tangency flips neither winding nor parity; a normal row
       through the body does enter the region. */
    if(strcmp(sc->name, "cubic_tangent_evenodd") == 0) {
        int wl, pl, wr, pr, wb, pb;
        rshape_ray_state(&sc->fill, 4.0, 0.5, &wl, &pl);
        rshape_ray_state(&sc->fill, 4.0, 15.5, &wr, &pr);
        CHECK(sc->name, wl == 0 && pl == 0 && wr == 0 && pr == 0,
            "tangent scanline leaked a crossing l=(%d,%d) r=(%d,%d)",
            wl, pl, wr, pr);
        rshape_ray_state(&sc->fill, 11.5, 8.0, &wb, &pb);
        CHECK(sc->name, pb == 1,
            "tangent body row not inside parity=%d", pb);
    }
}

/* ------------------------------------------------------------------ */
/* SRC_OVER exact premultiplied byte check                             */
/* ------------------------------------------------------------------ */

static void check_src_over(void)
{
    const char* name = "blend_src_over_rects";
    const int W = CANVAS, H = CANVAS;
    plutovg_surface_t* surf = plutovg_surface_create(W, H);
    plutovg_canvas_t* cv = plutovg_canvas_create(surf);
    plutovg_canvas_set_operator(cv, PLUTOVG_OPERATOR_SRC_OVER);

    const int r1[4] = { 2, 2, 8, 7 };
    const int r2[4] = { 6, 5, 8, 8 };
    const float c1[4] = { 0.8f, 0.4f, 0.2f, 0.5f };
    const float c2[4] = { 0.25f, 0.5f, 0.75f, 0.375f };

    plutovg_canvas_set_rgba(cv, c1[0], c1[1], c1[2], c1[3]);
    plutovg_canvas_rect(cv, r1[0], r1[1], r1[2], r1[3]);
    plutovg_canvas_fill(cv);
    plutovg_canvas_set_rgba(cv, c2[0], c2[1], c2[2], c2[3]);
    plutovg_canvas_rect(cv, r2[0], r2[1], r2[2], r2[3]);
    plutovg_canvas_fill(cv);

    int a1 = lroundf(c1[3] * 255);
    int a2 = lroundf(c2[3] * 255);
    int pre1[3] = { lroundf(c1[0] * a1), lroundf(c1[1] * a1),
        lroundf(c1[2] * a1) };
    int pre2[3] = { lroundf(c2[0] * a2), lroundf(c2[1] * a2),
        lroundf(c2[2] * a2) };
    int overlap_alpha = a2 + byte_mul(a1, 255 - a2);
    int mix[3];
    for(int ch = 0; ch < 3; ch++)
        mix[ch] = pre2[ch] + byte_mul(pre1[ch], 255 - a2);

    for(int y = 0; y < H; y++) {
        for(int x = 0; x < W; x++) {
            int in1 = x >= r1[0] && x < r1[0] + r1[2] &&
                      y >= r1[1] && y < r1[1] + r1[3];
            int in2 = x >= r2[0] && x < r2[0] + r2[2] &&
                      y >= r2[1] && y < r2[1] + r2[3];
            uint32_t px = pixel_argb(surf, x, y);
            int got[4] = { (int)((px >> 16) & 255), (int)((px >> 8) & 255),
                (int)(px & 255), (int)(px >> 24) };
            int want[4];
            if(in2) {
                if(in1) {
                    want[0] = mix[0]; want[1] = mix[1]; want[2] = mix[2];
                    want[3] = overlap_alpha;
                } else {
                    want[0] = pre2[0]; want[1] = pre2[1];
                    want[2] = pre2[2]; want[3] = a2;
                }
            } else if(in1) {
                want[0] = pre1[0]; want[1] = pre1[1];
                want[2] = pre1[2]; want[3] = a1;
            } else {
                want[0] = want[1] = want[2] = want[3] = 0;
            }
            for(int ch = 0; ch < 4; ch++)
                CHECK(name, abs(got[ch] - want[ch]) <= 2,
                    "pixel=(%d,%d) ch=%d expected=%d got=%d",
                    x, y, ch, want[ch], got[ch]);
        }
    }
    printf("FILL_SCENE %-28s independent SRC_OVER premult byte check\n",
        name);
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);
}

/* ------------------------------------------------------------------ */
/* Path consumption checks                                             */
/* ------------------------------------------------------------------ */

static void check_path_consumption(void)
{
    const char* name = "path_consumption";
    const int W = CANVAS, H = CANVAS;

    plutovg_surface_t* surf = plutovg_surface_create(W, H);
    plutovg_canvas_t* cv = plutovg_canvas_create(surf);
    plutovg_canvas_set_rgb(cv, 1, 1, 1);
    plutovg_canvas_rect(cv, 2, 2, 4, 4);
    plutovg_canvas_fill(cv);
    CHECK(name, pixel_alpha(surf, 3, 3) == 255, "first fill missing");
    plutovg_canvas_rect(cv, 10, 10, 4, 4);
    plutovg_canvas_fill(cv);
    CHECK(name, pixel_alpha(surf, 3, 3) == 255 &&
        pixel_alpha(surf, 11, 11) == 255,
        "consumed path was redrawn or second fill missing");
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);

    surf = plutovg_surface_create(W, H);
    cv = plutovg_canvas_create(surf);
    plutovg_canvas_set_rgb(cv, 1, 1, 1);
    plutovg_canvas_rect(cv, 1, 1, 4, 4);
    plutovg_canvas_fill_preserve(cv);
    CHECK(name, pixel_alpha(surf, 2, 2) == 255, "preserve fill missing");
    plutovg_canvas_translate(cv, 7, 7);
    plutovg_canvas_fill(cv);
    CHECK(name, pixel_alpha(surf, 9, 9) == 255,
        "preserved path not redrawn after translate");
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);

    surf = plutovg_surface_create(W, H);
    cv = plutovg_canvas_create(surf);
    plutovg_canvas_set_rgb(cv, 1, 1, 1);
    plutovg_path_t* ext = plutovg_path_create();
    plutovg_path_add_rect(ext, 2, 2, 4, 4);
    plutovg_canvas_rect(cv, 10, 10, 4, 4); /* pending canvas path */
    plutovg_canvas_fill_path(cv, ext);     /* draws ext, resets canvas path */
    CHECK(name, pixel_alpha(surf, 3, 3) == 255 &&
        pixel_alpha(surf, 11, 11) == 0,
        "fill_path did not draw the external path only");
    plutovg_canvas_fill(cv); /* canvas path was consumed: nothing new */
    CHECK(name, pixel_alpha(surf, 11, 11) == 0,
        "fill_path left content on the canvas path");
    plutovg_path_destroy(ext);
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);
    printf("FILL_SCENE %-28s fill/fill_preserve/fill_path consumption\n",
        name);
}

int main(int argc, char** argv)
{
    g_rep.outdir = argc > 1 ? argv[1] : ".";

    for(int i = 0; i < g_white_count; i++) {
        const white_scene_t* d = &g_white[i];
        scene_geom_t sc;
        memset(&sc, 0, sizeof(sc));
        set_base(&sc, d);

        plutovg_surface_t* surf = plutovg_surface_create(sc.w, sc.h);
        plutovg_canvas_t* cv = plutovg_canvas_create(surf);
        plutovg_canvas_set_rgb(cv, 1, 1, 1);
        plutovg_canvas_set_fill_rule(cv,
            sc.rule == RRULE_EVEN_ODD ? PLUTOVG_FILL_RULE_EVEN_ODD
                                      : PLUTOVG_FILL_RULE_NON_ZERO);
        d->draw(cv);
        g_rep.scenes_run++;

        probe_ray_invariants(&sc);
        compare_white(&sc, surf);

        plutovg_canvas_destroy(cv);
        plutovg_surface_destroy(surf);
        rshape_destroy(&sc.fill);
        for(int k = 0; k < MAX_CLIP_LAYERS; k++)
            rshape_destroy(&sc.clip[k]);
    }

    check_src_over();
    g_rep.scenes_run++;
    check_path_consumption();
    g_rep.scenes_run++;

    CHECK("global", g_rep.worst_line_stability <= 2.0 / 255.0,
        "line reference density drift %.5f > 2/255",
        g_rep.worst_line_stability);
    CHECK("global", g_rep.worst_curve_stability <= 2.0 / 255.0,
        "curve reference density drift %.5f > 2/255",
        g_rep.worst_curve_stability);

    printf("FILL_REFERENCE_CASES=%d\n", g_rep.scenes_run);
    printf("FILL_REFERENCE_ASSERTS=%ld\n", g_rep.asserts);
    printf("FILL_REFERENCE_MAX_CURVE_BAND=%.6f\n", g_rep.max_band);
    printf("FILL_REFERENCE_LINE_STABILITY=%.6f\n",
        g_rep.worst_line_stability);
    printf("FILL_REFERENCE_CURVE_STABILITY=%.6f\n",
        g_rep.worst_curve_stability);
    if(g_rep.failures == 0 && g_rep.scenes_run > 0 && g_rep.asserts > 0) {
        puts("FILL_REFERENCE_OK");
        return 0;
    }
    fprintf(stderr, "FILL_REFERENCE_FAILURE %s\n",
        g_rep.first_fail[0] ? g_rep.first_fail : "(no detail)");
    return 1;
}
