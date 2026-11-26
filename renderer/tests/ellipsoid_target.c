#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"

#include "renderer/core/ellipsoid.h"
#include "renderer/core/graphics.h"
#include "renderer/core/macro.h"
#include "renderer/core/maths.h"

/*
 * target cases for the analytic ellipsoid primitive in
 * renderer/core/ellipsoid.{h,c}
 *
 * every expectation here is produced in double precision from the defining
 * geometry, never by calling the implementation's intersection or inverse
 * helpers:
 *
 *   - depth: a self-contained per-pixel ray reference with its own 4x4
 *     inverse, its own naive quadratic solve and its own root selection;
 *   - surface: the defining relations world = A * local + t and
 *     normal = normalize((A^-1)^T * local) with an independently inverted A,
 *     plus exact hand-derived points sampled by a 1x1 orthographic probe;
 *   - pipeline: the framebuffer contract shared with triangles.
 */

/* ------------------------------------------------------------------------- */
/* double precision geometry                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {double x, y, z;} d3;
typedef struct {double x, y, z, w;} d4;

static d3 mk(double x, double y, double z) {
    d3 r; r.x = x; r.y = y; r.z = z; return r;
}

static d4 mk4(double x, double y, double z, double w) {
    d4 r; r.x = x; r.y = y; r.z = z; r.w = w; return r;
}

static d3 sub3(d3 a, d3 b) { return mk(a.x - b.x, a.y - b.y, a.z - b.z); }
static double dot3(d3 a, d3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double len3(d3 a) { return sqrt(dot3(a, a)); }
static double absd(double v) { return v < 0 ? -v : v; }
static double dist3(d3 a, d3 b) { return len3(sub3(a, b)); }

static d3 normalize3(d3 a) {
    double l = len3(a);
    return mk(a.x / l, a.y / l, a.z / l);
}

/* cofactor inverse of a 3x3 (double) */
static int inverse3(const double a[3][3], double inv[3][3]) {
    double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    double c10 = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    double c11 = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    double c12 = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    double c20 = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    double c21 = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    double c22 = a[0][0] * a[1][1] - a[0][1] * a[1][0];
    double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (absd(det) < 1e-30) return 0;
    inv[0][0] = c00 / det; inv[0][1] = c10 / det; inv[0][2] = c20 / det;
    inv[1][0] = c01 / det; inv[1][1] = c11 / det; inv[1][2] = c21 / det;
    inv[2][0] = c02 / det; inv[2][1] = c12 / det; inv[2][2] = c22 / det;
    return 1;
}

/* Gauss-Jordan inverse of a 4x4 (double), partial pivoting */
static int inverse4(const double a[4][4], double inv[4][4]) {
    double w[4][4], b[4][4];
    int i, j, r, c, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            w[i][j] = a[i][j];
            b[i][j] = (i == j) ? 1.0 : 0.0;
        }
    for (c = 0; c < 4; c++) {
        int piv = c;
        double div;
        for (r = c + 1; r < 4; r++)
            if (absd(w[r][c]) > absd(w[piv][c])) piv = r;
        if (absd(w[piv][c]) < 1e-14) return 0;
        if (piv != c)
            for (k = 0; k < 4; k++) {
                double t = w[c][k]; w[c][k] = w[piv][k]; w[piv][k] = t;
                t = b[c][k]; b[c][k] = b[piv][k]; b[piv][k] = t;
            }
        div = w[c][c];
        for (k = 0; k < 4; k++) { w[c][k] /= div; b[c][k] /= div; }
        for (r = 0; r < 4; r++) {
            double f;
            if (r == c) continue;
            f = w[r][c];
            for (k = 0; k < 4; k++) {
                w[r][k] -= f * w[c][k];
                b[r][k] -= f * b[c][k];
            }
        }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) inv[i][j] = b[i][j];
    return 1;
}

static d4 mul4(const double m[4][4], d4 v) {
    return mk4(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
               m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
               m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
               m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w);
}

/* float repo types <-> double locals */
static void mat_to_double(mat4_t m, double out[4][4]) {
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) out[i][j] = (double)m.m[i][j];
}

static mat4_t double_to_mat(const double m[4][4]) {
    mat4_t r;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) r.m[i][j] = (float)m[i][j];
    return r;
}

static d3 to_d3(vec3_t v) { return mk((double)v.x, (double)v.y, (double)v.z); }

static void linear_of(const double m[4][4], double a[3][3]) {
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) a[i][j] = m[i][j];
}

static d3 translation_of(const double m[4][4]) {
    return mk(m[0][3], m[1][3], m[2][3]);
}

static d3 apply_affine(const double a[3][3], d3 t, d3 p) {
    return mk(a[0][0] * p.x + a[0][1] * p.y + a[0][2] * p.z + t.x,
              a[1][0] * p.x + a[1][1] * p.y + a[1][2] * p.z + t.y,
              a[2][0] * p.x + a[2][1] * p.y + a[2][2] * p.z + t.z);
}

static d3 apply_linear(const double a[3][3], d3 p) {
    return mk(a[0][0] * p.x + a[0][1] * p.y + a[0][2] * p.z,
              a[1][0] * p.x + a[1][1] * p.y + a[1][2] * p.z,
              a[2][0] * p.x + a[2][1] * p.y + a[2][2] * p.z);
}

/* ------------------------------------------------------------------------- */
/* scenes                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    double object_to_world[4][4];
    double world_to_clip[4][4];
    d3 eye;
    int width, height;
} ref_scene;

static mat4_t make_view(d3 eye, d3 target) {
    return mat4_lookat(vec3_new((float)eye.x, (float)eye.y, (float)eye.z),
                       vec3_new((float)target.x, (float)target.y,
                                (float)target.z),
                       vec3_new(0, 1, 0));
}

/* perspective scene with near/far 0.5/10 and a 60 degree vertical fov */
static ref_scene make_scene(mat4_t object_to_world, d3 eye, d3 target,
                            int width, int height) {
    ref_scene s;
    mat4_t view = make_view(eye, target);
    mat4_t proj = mat4_perspective(TO_RADIANS(60),
                                   (float)width / (float)height, 0.5f, 10);
    mat_to_double(object_to_world, s.object_to_world);
    mat_to_double(mat4_mul_mat4(proj, view), s.world_to_clip);
    s.eye = eye;
    s.width = width;
    s.height = height;
    return s;
}

/*
 * orthographic 1x1 scene: the single pixel center is ndc (0, 0), so its eye
 * ray is exactly the eye -> target line. used for hand-derived surface points.
 */
static ref_scene make_probe_scene(mat4_t object_to_world, d3 eye) {
    ref_scene s;
    mat4_t view = make_view(eye, mk(eye.x, eye.y, eye.z - 1.0));
    mat4_t proj = mat4_orthographic(1.0f, 1.0f, 0.5f, 10.0f);
    mat_to_double(object_to_world, s.object_to_world);
    mat_to_double(mat4_mul_mat4(proj, view), s.world_to_clip);
    s.eye = eye;
    s.width = 1;
    s.height = 1;
    return s;
}

static ellipsoid_draw_t make_draw(const ref_scene *s,
                                  ellipsoid_shader_t *shader,
                                  void *uniforms, int blend) {
    ellipsoid_draw_t d;
    d.object_to_world = double_to_mat((const double (*)[4])s->object_to_world);
    d.world_to_clip = double_to_mat((const double (*)[4])s->world_to_clip);
    d.shader = shader;
    d.uniforms = uniforms;
    d.enable_blend = blend;
    return d;
}

/* ------------------------------------------------------------------------- */
/* independent per-pixel ray reference                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    int hit;
    int backface;
    d3 local;
    d3 world;
    d3 normal;
    double depth;
    double t;
    int r0_on_segment;
    int r1_on_segment;
    double depth_r0;
    double depth_r1;
} ref_hit;

static double reproject_depth_from(const double clip[4][4], d3 world) {
    d4 c = mul4((const double (*)[4])clip,
                mk4(world.x, world.y, world.z, 1.0));
    return absd(c.w) > 1e-12 ? (c.z / c.w + 1.0) * 0.5 : -1.0;
}

static ref_hit reference_pixel(const ref_scene *s, int px, int py) {
    ref_hit out;
    double inv_clip[4][4], obj_lin[3][3], inv_obj[3][3], inv_t[3][3];
    d3 obj_trans, near_w, far_w, near_o, far_o, dir_o;
    double xf, yf, qa, qb, qc, disc, r0, r1, t;
    int i, j;

    memset(&out, 0, sizeof(out));
    if (!inverse4((const double (*)[4])s->world_to_clip, inv_clip)) return out;
    linear_of((const double (*)[4])s->object_to_world, obj_lin);
    obj_trans = translation_of((const double (*)[4])s->object_to_world);
    if (!inverse3(obj_lin, inv_obj)) return out;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) inv_t[i][j] = inv_obj[j][i];

    xf = 2.0 * ((double)px + 0.5) / (double)s->width - 1.0;
    yf = 2.0 * ((double)py + 0.5) / (double)s->height - 1.0;
    {
        d4 hn = mul4(inv_clip, mk4(xf, yf, -1.0, 1.0));
        d4 hf = mul4(inv_clip, mk4(xf, yf, 1.0, 1.0));
        if (absd(hn.w) < 1e-12 || absd(hf.w) < 1e-12) return out;
        near_w = mk(hn.x / hn.w, hn.y / hn.w, hn.z / hn.w);
        far_w = mk(hf.x / hf.w, hf.y / hf.w, hf.z / hf.w);
    }

    /* object space segment: A^-1 * (w - t), translation folded in */
    {
        d3 it = mk(-(inv_obj[0][0] * obj_trans.x + inv_obj[0][1] * obj_trans.y +
                     inv_obj[0][2] * obj_trans.z),
                   -(inv_obj[1][0] * obj_trans.x + inv_obj[1][1] * obj_trans.y +
                     inv_obj[1][2] * obj_trans.z),
                   -(inv_obj[2][0] * obj_trans.x + inv_obj[2][1] * obj_trans.y +
                     inv_obj[2][2] * obj_trans.z));
        near_o = apply_affine(inv_obj, it, near_w);
        far_o = apply_affine(inv_obj, it, far_w);
    }
    dir_o = sub3(far_o, near_o);

    qa = dot3(dir_o, dir_o);
    qb = 2.0 * dot3(near_o, dir_o);
    qc = dot3(near_o, near_o) - 1.0;
    if (!(qa > 1e-30)) return out;
    disc = qb * qb - 4.0 * qa * qc;
    if (!(disc >= 0.0)) return out;
    r0 = (-qb - sqrt(disc)) / (2.0 * qa);
    r1 = (-qb + sqrt(disc)) / (2.0 * qa);

    if (qc <= 0.0) {
        if (!(r1 >= 0.0 && r1 <= 1.0)) return out;
        t = r1;
        out.backface = 1;
    } else {
        if (r0 < 0.0 || r0 > 1.0) return out;
        t = r0;
        out.backface = 0;
    }

    out.hit = 1;
    out.t = t;
    out.local = normalize3(mk(near_o.x + t * dir_o.x,
                              near_o.y + t * dir_o.y,
                              near_o.z + t * dir_o.z));
    out.world = apply_affine(obj_lin, obj_trans, out.local);
    out.normal = normalize3(apply_linear(inv_t, out.local));
    out.depth = reproject_depth_from((const double (*)[4])s->world_to_clip,
                                     out.world);
    /* the implementation clips to the NDC box including depth; a point
     * projecting outside it is not drawn */
    {
        d4 c = mul4((const double (*)[4])s->world_to_clip,
                    mk4(out.world.x, out.world.y, out.world.z, 1.0));
        double nx = c.x / c.w, ny = c.y / c.w, nz = c.z / c.w;
        if (!(absd(nx) <= 1.0 + 1e-5) || !(absd(ny) <= 1.0 + 1e-5) ||
            !(nz >= -1.0 - 1e-5 && nz <= 1.0 + 1e-5)) {
            return out;   /* hit stays 0 */
        }
    }

    out.r0_on_segment = (r0 >= 0.0 && r0 <= 1.0);
    out.r1_on_segment = (r1 >= 0.0 && r1 <= 1.0);
    out.depth_r0 = reproject_depth_from((const double (*)[4])s->world_to_clip,
                                        apply_affine(obj_lin, obj_trans,
                                                     normalize3(mk(
                                                         near_o.x + r0 * dir_o.x,
                                                         near_o.y + r0 * dir_o.y,
                                                         near_o.z + r0 * dir_o.z))));
    out.depth_r1 = reproject_depth_from((const double (*)[4])s->world_to_clip,
                                        apply_affine(obj_lin, obj_trans,
                                                     normalize3(mk(
                                                         near_o.x + r1 * dir_o.x,
                                                         near_o.y + r1 * dir_o.y,
                                                         near_o.z + r1 * dir_o.z))));
    return out;
}

/* ------------------------------------------------------------------------- */
/* shaders and records                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    int calls;
    int front_calls;
    int back_calls;
    int discard_mask;
    int discards;
    ellipsoid_hit_t last;
} record_t;

static vec4_t record_shader(const ellipsoid_hit_t *hit, void *uniforms,
                            int *discard) {
    record_t *r = (record_t*)uniforms;
    r->calls++;
    if (hit->backface) r->back_calls++;
    else r->front_calls++;
    r->last = *hit;
    if (r->discard_mask) {
        r->discards++;
        *discard = 1;
    }
    return vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
}

typedef struct {
    vec4_t color;
    int discard;
    int calls;
} color_uniforms_t;

static vec4_t color_shader(const ellipsoid_hit_t *hit, void *uniforms,
                           int *discard) {
    color_uniforms_t *u = (color_uniforms_t*)uniforms;
    (void)hit;
    u->calls++;
    if (u->discard) *discard = 1;
    return u->color;
}

/* tolerates NULL uniforms; returns red and counts only when non-NULL */
static vec4_t boom_shader(const ellipsoid_hit_t *hit, void *uniforms,
                          int *discard) {
    (void)hit; (void)discard;
    if (uniforms != NULL) (*(int*)uniforms)++;
    return vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
}

/*
 * checks the defining surface relations straight from the hit fields; the
 * expected world / normal / depth are recomputed here from the matrices.
 */
typedef struct {
    double obj_lin[3][3];
    double obj_trans[3];
    double inv_trans[3][3];
    double world_to_clip[4][4];
    d3 eye;
    int calls;
    int front_calls;
    int back_calls;
    int local_off_sphere;
    int world_off_surface;
    int normal_wrong;
    int normal_not_unit;
    int front_normal_away_from_eye;
    int back_normal_toward_eye;
    int depth_wrong;
} verify_t;

static vec4_t verify_shader(const ellipsoid_hit_t *hit, void *uniforms,
                            int *discard) {
    verify_t *u = (verify_t*)uniforms;
    d3 lp = to_d3(hit->local_position);
    d3 wp = to_d3(hit->world_position);
    d3 wn = to_d3(hit->world_normal);
    d3 expect_world, expect_normal;
    double expect_depth;
    u->calls++;
    if (hit->backface) u->back_calls++; else u->front_calls++;

    if (absd(len3(lp) - 1.0) > 2e-5) u->local_off_sphere++;

    expect_world = apply_affine(u->obj_lin,
                                mk(u->obj_trans[0], u->obj_trans[1],
                                   u->obj_trans[2]), lp);
    if (dist3(expect_world, wp) > 2e-4) u->world_off_surface++;

    expect_normal = normalize3(apply_linear(u->inv_trans, lp));
    if (absd(len3(wn) - 1.0) > 2e-4) u->normal_not_unit++;
    if (dist3(expect_normal, wn) > 2e-4) u->normal_wrong++;

    /* outward: from outside it points back to the eye, from inside away */
    if (!hit->backface && dot3(wn, sub3(u->eye, wp)) <= 0.0)
        u->front_normal_away_from_eye++;
    if (hit->backface && dot3(wn, sub3(wp, u->eye)) <= 0.0)
        u->back_normal_toward_eye++;

    expect_depth = reproject_depth_from(u->world_to_clip, wp);
    if (absd((double)hit->depth - expect_depth) > 2e-4) u->depth_wrong++;

    *discard = 0;
    return vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
}

static void verify_init(verify_t *v, const ref_scene *s) {
    int i, j;
    memset(v, 0, sizeof(*v));
    linear_of((const double (*)[4])s->object_to_world, v->obj_lin);
    {
        d3 t = translation_of((const double (*)[4])s->object_to_world);
        v->obj_trans[0] = t.x;
        v->obj_trans[1] = t.y;
        v->obj_trans[2] = t.z;
    }
    {
        double inv[3][3];
        inverse3(v->obj_lin, inv);
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++) v->inv_trans[i][j] = inv[j][i];
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            v->world_to_clip[i][j] = s->world_to_clip[i][j];
    v->eye = s->eye;
}

/* ------------------------------------------------------------------------- */
/* old triangle program path                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {vec4_t pos;} tri_attrib_t;

static vec4_t tri_vs(void *attribs, void *varyings, void *uniforms) {
    (void)varyings; (void)uniforms;
    return ((tri_attrib_t*)attribs)->pos;
}

static vec4_t tri_fs(void *varyings, void *uniforms, int *discard,
                     int backface) {
    (void)varyings; (void)backface;
    if (uniforms != NULL && *(int*)uniforms == 1) *discard = 1;
    return vec4_new(0.0f, 0.0f, 1.0f, 1.0f);
}

static void set_triangle(program_t *p, float z) {
    tri_attrib_t *a0 = (tri_attrib_t*)program_get_attribs(p, 0);
    tri_attrib_t *a1 = (tri_attrib_t*)program_get_attribs(p, 1);
    tri_attrib_t *a2 = (tri_attrib_t*)program_get_attribs(p, 2);
    a0->pos = vec4_new(-1.0f, -1.0f, z, 1.0f);
    a1->pos = vec4_new( 3.0f, -1.0f, z, 1.0f);
    a2->pos = vec4_new(-1.0f,  3.0f, z, 1.0f);
}

/* ------------------------------------------------------------------------- */
/* frame helpers                                                             */
/* ------------------------------------------------------------------------- */

static void clear_frame(framebuffer_t *fb) {
    framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(fb, 1);
}

static double pixel_depth(framebuffer_t *fb, int x, int y) {
    return (double)fb->depth_buffer[y * fb->width + x];
}

static int pixel_channel(framebuffer_t *fb, int x, int y, int c) {
    return (int)fb->color_buffer[(y * fb->width + x) * 4 + c];
}

/* ========================================================================= */
/* depth group                                                               */
/* ========================================================================= */

/*
 * shaded pixels and depths must agree with the independent reference on the
 * whole frame; a pixel covered by the screen rectangle but missing the sphere
 * must keep cleared depth/colors and must not call the shader.
 */
static int depth_matches_reference(void) {
    framebuffer_t *fb = framebuffer_create(48, 48);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             48, 48);
    record_t rec;
    ellipsoid_draw_t d;
    int x, y, ref_hits = 0, depth_bad = 0, stray = 0;

    memset(&rec, 0, sizeof(rec));
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    for (y = 0; y < 48; y++) {
        for (x = 0; x < 48; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            if (rh.hit) {
                ref_hits++;
                if (absd(pixel_depth(fb, x, y) - rh.depth) > 3e-4) depth_bad++;
            } else if (fb->depth_buffer[y * 48 + x] != 1.0f) {
                stray++;
            }
        }
    }
    TASK_CHECK(depth_bad == 0);
    TASK_CHECK(stray == 0);
    TASK_CHECK(ref_hits > 100);
    /* the shader runs once per real surface pixel (silhouette aside) */
    TASK_CHECK(abs(rec.calls - ref_hits) <= 2);
    /* a bbox corner covering no surface keeps its cleared pixel */
    TASK_CHECK(fb->depth_buffer[0] == 1.0f && fb->color_buffer[0] == 0);
    framebuffer_release(fb);
    return 0;
}

/*
 * root ordering: with both roots on the segment the nearer (smaller) one is
 * shaded; a camera outside must never pick an ambiguous far root.
 */
static int depth_root_selection(void) {
    framebuffer_t *fb = framebuffer_create(40, 40);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             40, 40);
    record_t rec;
    ellipsoid_draw_t d;
    int x, y, checked = 0, both = 0;

    memset(&rec, 0, sizeof(rec));
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    for (y = 0; y < 40; y++) {
        for (x = 0; x < 40; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            double got;
            if (!rh.hit) continue;
            checked++;
            got = pixel_depth(fb, x, y);
            if (rh.r0_on_segment && rh.r1_on_segment) {
                both++;
                TASK_CHECK(got < rh.depth_r1 - 1e-5);
            }
            TASK_CHECK(absd(got - rh.depth_r0) <= 3e-4);
            TASK_CHECK(got >= 0.0 && got <= 1.0);
        }
    }
    TASK_CHECK(checked > 150);
    TASK_CHECK(both > 100);      /* the cut sphere exposes two roots */
    TASK_CHECK(rec.back_calls == 0);
    framebuffer_release(fb);
    return 0;
}

/*
 * near plane cutting into the sphere: every visible hit is the exit surface,
 * sampled at the real curved point, never a flat cap at depth 0.
 */
static int depth_near_plane_cut(void) {
    framebuffer_t *fb = framebuffer_create(32, 32);
    ref_scene s = make_scene(mat4_scale(1.6f, 1.6f, 1.6f), mk(0, 0, 1.0),
                             mk(0, 0, 0), 32, 32);
    record_t rec;
    ellipsoid_draw_t d;
    int x, y, checked = 0, bad = 0, cap = 0, all_back = 1;
    double lo = 2.0, hi = -1.0;

    memset(&rec, 0, sizeof(rec));
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            double got;
            if (!rh.hit) continue;
            checked++;
            got = pixel_depth(fb, x, y);
            if (absd(got - rh.depth) > 3e-4) bad++;
            if (got <= 1e-4) cap++;
            if (!rh.backface) all_back = 0;
            if (got < lo) lo = got;
            if (got > hi) hi = got;
        }
    }
    TASK_CHECK(checked > 100);
    TASK_CHECK(bad == 0);
    TASK_CHECK(cap == 0);        /* no near-plane cap */
    TASK_CHECK(all_back);
    TASK_CHECK(rec.back_calls == rec.calls);
    TASK_CHECK(hi - lo > 0.01);  /* the exit surface really varies */
    framebuffer_release(fb);
    return 0;
}

/*
 * a closed ellipsoid in front of / behind a triangle uses the same depth
 * buffer and the same <= rule as the triangle pipeline.
 */
static int depth_triangle_occlusion(void) {
    framebuffer_t *fb = framebuffer_create(32, 32);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             32, 32);
    record_t rec;
    ellipsoid_draw_t d;
    program_t *tri;
    ref_hit center = reference_pixel(&s, 16, 16);

    TASK_CHECK(center.hit);

    /* closed triangle in front: the sphere must not overwrite it */
    clear_frame(fb);
    tri = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                         (int)sizeof(float), (int)sizeof(int), 0, 0);
    *(int*)program_get_uniforms(tri) = 0;
    set_triangle(tri, -0.96f);           /* depth ~0.02, in front */
    graphics_draw_triangle(fb, tri);
    memset(&rec, 0, sizeof(rec));
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(rec.calls == 0);
    TASK_CHECK(pixel_channel(fb, 16, 16, 2) == 255);  /* triangle blue kept */
    TASK_CHECK(pixel_depth(fb, 16, 16) <= 0.02 + 1e-5);
    program_release(tri);

    /* triangle behind: the sphere wins and reports the reference depth */
    clear_frame(fb);
    tri = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                         (int)sizeof(float), (int)sizeof(int), 0, 0);
    *(int*)program_get_uniforms(tri) = 0;
    set_triangle(tri, 0.999f);           /* depth ~0.9995, behind */
    graphics_draw_triangle(fb, tri);
    memset(&rec, 0, sizeof(rec));
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(rec.calls > 50);
    TASK_CHECK(pixel_channel(fb, 16, 16, 0) == 255);  /* sphere red */
    TASK_CHECK(absd(pixel_depth(fb, 16, 16) - center.depth) <= 3e-4);
    program_release(tri);

    /* sanity: the sphere spans a range of NDC depths, not a constant plane */
    {
        int shallow = 0, deep = 0;
        int x, y;
        double lo = 2.0, hi = -1.0, mid;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) continue;
                if (rh.depth < lo) lo = rh.depth;
                if (rh.depth > hi) hi = rh.depth;
            }
        mid = (lo + hi) * 0.5;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) continue;
                if (rh.depth < mid) shallow++; else deep++;
            }
        TASK_CHECK(hi - lo > 0.01);
        TASK_CHECK(shallow > 10 && deep > 10);
    }
    framebuffer_release(fb);
    return 0;
}

/*
 * the same occlusion question asked through the framebuffer rather than the
 * reference: a plane placed midway through the sphere's NDC depth range must
 * hide exactly the pixels whose reference depth is behind it.
 */
static int depth_triangle_midplane(void) {
    framebuffer_t *fb = framebuffer_create(32, 32);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             32, 32);
    program_t *tri;
    float plane_ndc;
    double lo = 2.0, hi = -1.0, mid;
    int x, y, blocked = 0, open = 0, bad = 0;

    /* choose the plane midway through the sphere's actual NDC depth span */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            if (!rh.hit) continue;
            if (rh.depth < lo) lo = rh.depth;
            if (rh.depth > hi) hi = rh.depth;
        }
    TASK_CHECK(hi - lo > 0.01);
    mid = (lo + hi) * 0.5;
    plane_ndc = (float)(mid * 2.0 - 1.0);   /* depth -> ndc z */

    clear_frame(fb);
    tri = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                         (int)sizeof(float), (int)sizeof(int), 0, 0);
    *(int*)program_get_uniforms(tri) = 0;
    set_triangle(tri, plane_ndc);
    graphics_draw_triangle(fb, tri);

    {
        ellipsoid_draw_t d = make_draw(&s, boom_shader, NULL, 0);
        TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    }
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            double got = pixel_depth(fb, x, y);
            if (!rh.hit)
                continue;
            if (rh.depth < mid) {
                blocked++;   /* in front of the plane: sphere overwrote it */
                if (absd(got - rh.depth) > 3e-4) bad++;
            } else {
                open++;      /* behind the plane: plane depth survives */
                if (absd(got - mid) > 1e-4) bad++;
            }
        }
    TASK_CHECK(blocked > 10 && open > 10);
    TASK_CHECK(bad == 0);
    program_release(tri);
    framebuffer_release(fb);
    return 0;
}

/* ========================================================================= */
/* surface group                                                             */
/* ========================================================================= */

/* draw into a 1x1 frame and return the single recorded hit */
static ellipsoid_hit_t probe_hit(ref_scene *s, int *ok) {
    framebuffer_t *fb = framebuffer_create(1, 1);
    record_t rec;
    ellipsoid_draw_t d;
    ellipsoid_hit_t hit;
    memset(&rec, 0, sizeof(rec));
    d = make_draw(s, record_shader, &rec, 0);
    graphics_draw_ellipsoid(fb, &d);
    hit = rec.last;
    *ok = (rec.calls == 1);
    framebuffer_release(fb);
    return hit;
}

/* run one perspective scene through the shader-side verifier */
static int verify_scene(const ref_scene *s, int expect_backface) {
    framebuffer_t *fb = framebuffer_create(s->width, s->height);
    verify_t v;
    ellipsoid_draw_t d;
    int x, y, ref_hits = 0, surface_bad = 0;

    verify_init(&v, s);
    d = make_draw(s, verify_shader, &v, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    TASK_CHECK(v.calls > 50);
    TASK_CHECK(v.local_off_sphere == 0);
    TASK_CHECK(v.world_off_surface == 0);
    TASK_CHECK(v.normal_wrong == 0);
    TASK_CHECK(v.normal_not_unit == 0);
    TASK_CHECK(v.front_normal_away_from_eye == 0);
    TASK_CHECK(v.back_normal_toward_eye == 0);
    TASK_CHECK(v.depth_wrong == 0);
    if (expect_backface >= 0) {
        if (expect_backface) TASK_CHECK(v.back_calls == v.calls);
        else TASK_CHECK(v.front_calls == v.calls);
    }

    for (y = 0; y < s->height; y++)
        for (x = 0; x < s->width; x++) {
            ref_hit rh = reference_pixel(s, x, y);
            if (!rh.hit) continue;
            ref_hits++;
            if (absd(pixel_depth(fb, x, y) - rh.depth) > 3e-4) surface_bad++;
        }
    TASK_CHECK(ref_hits > 50);
    TASK_CHECK(surface_bad == 0);
    framebuffer_release(fb);
    return 0;
}

/* non-uniform scale: positions and normals from the defining relations */
static int surface_nonuniform_scale(void) {
    mat4_t obj = mat4_mul_mat4(mat4_translate(0.3f, -0.2f, 0.5f),
                               mat4_scale(1.7f, 0.6f, 1.1f));
    ref_scene s = make_scene(obj, mk(1.5, 0.8, 4.5), mk(0.3, -0.2, 0.5),
                             40, 40);
    TASK_CHECK(verify_scene(&s, 0) == 0);
    /* the world shape is the scaled ellipsoid, not the unit sphere */
    {
        int x, y, off_unit = 0, hits = 0;
        d3 t = translation_of((const double (*)[4])s.object_to_world);
        for (y = 0; y < 40; y++)
            for (x = 0; x < 40; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) continue;
                hits++;
                if (dist3(rh.world, t) > 1.05) off_unit++;
            }
        TASK_CHECK(hits > 100);
        TASK_CHECK(off_unit > 30);
    }
    return 0;
}

/*
 * shear, hand-derived.  A = [[1,0,0.5],[0,1,0],[0,0,1]] around the origin.
 * the probe ray is world (0, 0, z).  pulling it back with A^-1 gives
 * local = (-0.5 z, 0, z); |local| = 1 => z = 0.8944272, so
 *   local  = (-0.4472136, 0, 0.8944272)
 *   world  = (0, 0, 0.8944272)
 *   normal = normalize((A^-1)^T local) = (-0.3713907, 0, 0.9284767)
 * (A^-1)^T local is (-0.4472136, 0, 1.1180340) before normalisation.
 */
static int surface_shear(void) {
    mat4_t obj = mat4_identity();
    int i, j;
    obj.m[0][2] = 0.5f;   /* x += 0.5 * z */
    (void)i; (void)j;

    {
        ref_scene probe = make_probe_scene(obj, mk(0, 0, 5));
        ellipsoid_hit_t hit;
        d3 expected_local = mk(-0.4472136, 0.0, 0.8944272);
        d3 expected_world = mk(0.0, 0.0, 0.8944272);
        d3 expected_normal = mk(-0.3713907, 0.0, 0.9284767);
        int ok = 0;

        hit = probe_hit(&probe, &ok);
        TASK_CHECK(ok);
        TASK_CHECK(dist3(to_d3(hit.local_position), expected_local) < 2e-4);
        TASK_CHECK(dist3(to_d3(hit.world_position), expected_world) < 2e-4);
        TASK_CHECK(dist3(to_d3(hit.world_normal), expected_normal) < 2e-4);
        /* A * local would give a normal along z; inverse transpose tilts x */
        TASK_CHECK(apply_linear((const double[3][3]){{1,0,0.5},{0,1,0},{0,0,1}},
                                expected_local).x != expected_normal.x ||
                   absd(expected_normal.x) > 0.2);
        TASK_CHECK(hit.backface == 0);
    }
    {
        ref_scene s = make_scene(obj, mk(0.2, 0.3, 5.0), mk(0, 0, 0), 40, 40);
        TASK_CHECK(verify_scene(&s, 0) == 0);
    }
    return 0;
}

/*
 * reflection with translation, hand-derived.  A = diag(-1, 1, 1), t = (0.5,0,0).
 * the probe ray is world (0, 0, z); local = A^-1 (world - t) = (0.5, 0, z)
 * and |local| = 1 gives z = sqrt(3)/2 = 0.8660254:
 *   local  = (0.5, 0, 0.8660254)
 *   world  = (0, 0, 0.8660254)
 *   normal = normalize(A^-T local) = (-0.5, 0, 0.8660254)
 */
static int surface_reflection(void) {
    mat4_t obj = mat4_mul_mat4(mat4_translate(0.5f, 0, 0),
                               mat4_scale(-1.0f, 1.0f, 1.0f));
    {
        ref_scene probe = make_probe_scene(obj, mk(0, 0, 5));
        ellipsoid_hit_t hit;
        double s3 = 0.86602540378;
        d3 expected_local = mk(0.5, 0.0, s3);
        d3 expected_world = mk(0.0, 0.0, s3);
        d3 expected_normal = mk(-0.5, 0.0, s3);
        int ok = 0;

        hit = probe_hit(&probe, &ok);
        TASK_CHECK(ok);
        TASK_CHECK(dist3(to_d3(hit.local_position), expected_local) < 2e-4);
        TASK_CHECK(dist3(to_d3(hit.world_position), expected_world) < 2e-4);
        TASK_CHECK(dist3(to_d3(hit.world_normal), expected_normal) < 2e-4);
        /* the reflected outward normal faces the eye: still the front side */
        TASK_CHECK(dot3(expected_normal, mk(0.0, 0.0, 5.0)) > 0);
        TASK_CHECK(hit.backface == 0);
    }
    {
        ref_scene s = make_scene(obj, mk(0, 0.4, 5.0), mk(0.5, 0, 0), 40, 40);
        TASK_CHECK(verify_scene(&s, 0) == 0);
    }
    return 0;
}

/*
 * camera inside the ellipsoid: every hit is the exit point (backface), still a
 * real sphere point, with outward normals pointing away from the eye.
 */
static int surface_camera_inside(void) {
    ref_scene s = make_scene(mat4_scale(1.5f, 1.5f, 1.5f), mk(0, 0, 0),
                             mk(0, 0, -1), 40, 40);
    framebuffer_t *fb = framebuffer_create(40, 40);
    verify_t v;
    ellipsoid_draw_t d;
    int x, y, hits = 0, back_bad = 0;

    verify_init(&v, &s);
    d = make_draw(&s, verify_shader, &v, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    TASK_CHECK(v.calls > 50);
    TASK_CHECK(v.local_off_sphere == 0);
    TASK_CHECK(v.world_off_surface == 0);
    TASK_CHECK(v.normal_wrong == 0);
    TASK_CHECK(v.normal_not_unit == 0);
    TASK_CHECK(v.back_normal_toward_eye == 0);
    TASK_CHECK(v.back_calls == v.calls);   /* all exit points */

    for (y = 0; y < 40; y++)
        for (x = 0; x < 40; x++) {
            ref_hit rh = reference_pixel(&s, x, y);
            if (!rh.hit) continue;
            hits++;
            if (!rh.backface) back_bad++;
            TASK_CHECK(pixel_depth(fb, x, y) >= 0.0 &&
                       pixel_depth(fb, x, y) <= 1.0);
            TASK_CHECK(absd(pixel_depth(fb, x, y) - rh.depth) <= 3e-4);
        }
    TASK_CHECK(hits > 100);
    TASK_CHECK(back_bad == 0);
    framebuffer_release(fb);
    return 0;
}

/* ========================================================================= */
/* pipeline group                                                            */
/* ========================================================================= */

/*
 * discard: the callback still runs for every real hit, but the framebuffer
 * color and depth stay untouched.
 */
static int pipeline_discard(void) {
    framebuffer_t *fb = framebuffer_create(24, 24);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             24, 24);
    record_t rec;
    ellipsoid_draw_t d;
    int x, y, hits = 0, depth_written = 0, color_written = 0;

    clear_frame(fb);
    memset(&rec, 0, sizeof(rec));
    rec.discard_mask = 1;
    d = make_draw(&s, record_shader, &rec, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);

    for (y = 0; y < 24; y++)
        for (x = 0; x < 24; x++) {
            if (!reference_pixel(&s, x, y).hit) continue;
            hits++;
            if (fb->depth_buffer[y * 24 + x] != 1.0f) depth_written++;
            if (pixel_channel(fb, x, y, 0) != 0) color_written++;
        }
    TASK_CHECK(hits > 50);
    TASK_CHECK(rec.calls == hits);
    TASK_CHECK(rec.discards == hits);
    TASK_CHECK(depth_written == 0);
    TASK_CHECK(color_written == 0);
    framebuffer_release(fb);
    return 0;
}

/*
 * blending: RGB is blended with the 0.5 source alpha exactly as the triangle
 * path does, and the destination alpha byte keeps its old value.
 */
static int pipeline_blend(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             16, 16);
    color_uniforms_t u;
    ellipsoid_draw_t d;
    int x, y;
    ref_hit center = reference_pixel(&s, 8, 8);

    TASK_CHECK(center.hit);

    /* destination opaque blue, half-transparent red sphere on top */
    framebuffer_clear_color(fb, vec4_new(0.0f, 0.0f, 1.0f, 1.0f));
    framebuffer_clear_depth(fb, 1);
    memset(&u, 0, sizeof(u));
    u.color = vec4_new(1.0f, 0.0f, 0.0f, 0.5f);
    d = make_draw(&s, color_shader, &u, 1);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(u.calls > 10);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            if (!reference_pixel(&s, x, y).hit) continue;
            TASK_CHECK(pixel_channel(fb, x, y, 0) == 127);  /* red half */
            TASK_CHECK(pixel_channel(fb, x, y, 2) == 127);  /* blue half */
        }
    TASK_CHECK(pixel_channel(fb, 8, 8, 3) == 255);   /* alpha preserved */

    /* destination alpha 0 survives blending */
    framebuffer_clear_color(fb, vec4_new(0.0f, 0.0f, 1.0f, 0.0f));
    framebuffer_clear_depth(fb, 1);
    memset(&u, 0, sizeof(u));
    u.color = vec4_new(1.0f, 0.0f, 0.0f, 0.5f);
    d = make_draw(&s, color_shader, &u, 1);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(pixel_channel(fb, 8, 8, 3) == 0);
    TASK_CHECK(pixel_channel(fb, 8, 8, 0) == 127);

    /* opaque, no blending: the saturated source lands verbatim */
    framebuffer_clear_color(fb, vec4_new(0.0f, 0.0f, 0.0f, 1.0f));
    framebuffer_clear_depth(fb, 1);
    memset(&u, 0, sizeof(u));
    u.color = vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
    d = make_draw(&s, color_shader, &u, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(pixel_channel(fb, 8, 8, 0) == 255);
    TASK_CHECK(pixel_channel(fb, 8, 8, 1) == 0);
    framebuffer_release(fb);
    return 0;
}

/*
 * depth equality: "depth <= depth_buffer" accepts the primitive's own depth,
 * so a fresh ellipsoid drawn over an identical depth is still shaded, while a
 * strictly larger stored depth is rejected and a smaller one accepts.
 */
static int pipeline_equal_depth(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             16, 16);
    color_uniforms_t u;
    ellipsoid_draw_t d;
    ref_hit center = reference_pixel(&s, 8, 8);
    int x, y;

    TASK_CHECK(center.hit);
    memset(&u, 0, sizeof(u));
    u.color = vec4_new(0.0f, 1.0f, 0.0f, 1.0f);
    d = make_draw(&s, color_shader, &u, 0);

    /* prefill with the would-be center depth: "depth <= depth_buffer" accepts
     * an equal depth, so a fresh ellipsoid is still shaded there */
    {
        double lo = 2.0, hi = -1.0, mid;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) continue;
                if (rh.depth < lo) lo = rh.depth;
                if (rh.depth > hi) hi = rh.depth;
            }
        mid = (lo + hi) * 0.5;
        framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                fb->depth_buffer[y * 16 + x] = (float)mid;
        TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
        {
            int front = 0, behind = 0;
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    ref_hit rh = reference_pixel(&s, x, y);
                    double got = pixel_depth(fb, x, y);
                    if (!rh.hit) {
                        TASK_CHECK(got == (float)mid);
                        continue;
                    }
                    if (rh.depth <= mid) {
                        front++;
                        TASK_CHECK(absd(got - rh.depth) <= 3e-4);
                    } else {
                        behind++;
                        TASK_CHECK(got == (float)mid);
                    }
                }
            TASK_CHECK(front > 10 && behind > 10);
        }
    }

    /* a stored depth larger than every sphere depth rejects everything */
    {
        double deepest = -1.0;
        int drawn = 0;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (rh.hit && rh.depth > deepest) deepest = rh.depth;
            }
        TASK_CHECK(deepest > 0.0);
        framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                fb->depth_buffer[y * 16 + x] = (float)(deepest + 0.05);
        TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                if (pixel_channel(fb, x, y, 1) != 0) drawn++;
        TASK_CHECK(drawn == 0);  /* strictly deeper: depth test rejected */
    }

    /* a strict depth buffer smaller than every sphere depth accepts all */
    {
        double nearest = 2.0;
        int drawn = 0, hits = 0;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (rh.hit && rh.depth < nearest) nearest = rh.depth;
            }
        TASK_CHECK(nearest < 1.0);
        framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                fb->depth_buffer[y * 16 + x] = (float)(nearest - 0.05);
        TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) continue;
                hits++;
                if (pixel_channel(fb, x, y, 1) == 255) drawn++;
            }
        TASK_CHECK(hits > 0);
        TASK_CHECK(drawn == hits);
    }
    framebuffer_release(fb);
    return 0;
}

/*
 * invalid input is rejected before the shader runs and before any buffer
 * changes; uniforms may be NULL.
 */
static int pipeline_invalid_inputs(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    ref_scene good = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                                16, 16);
    ellipsoid_draw_t base, d;
    color_uniforms_t u;
    unsigned char *color_copy;
    float *depth_copy;
    int size, calls = 0;

    /* establish valid content first */
    memset(&u, 0, sizeof(u));
    u.color = vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
    base = make_draw(&good, color_shader, &u, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &base) == 0);

    size = 16 * 16 * 4;
    color_copy = (unsigned char*)malloc((size_t)size);
    depth_copy = (float*)malloc((size_t)16 * 16 * sizeof(float));
    TASK_CHECK(color_copy != NULL && depth_copy != NULL);
    memcpy(color_copy, fb->color_buffer, (size_t)size);
    memcpy(depth_copy, fb->depth_buffer, (size_t)16 * 16 * sizeof(float));

    base = make_draw(&good, boom_shader, &calls, 0);

    /* NULL framebuffer / draw / shader */
    TASK_CHECK(graphics_draw_ellipsoid(NULL, &base) != 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, NULL) != 0);
    d = base;
    d.shader = NULL;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);

    /* non-finite matrices */
    d = base;
    d.object_to_world.m[0][0] = (float)NAN;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);
    d = base;
    d.object_to_world.m[1][2] = (float)INFINITY;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);
    d = base;
    d.world_to_clip.m[2][3] = (float)NAN;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);

    /* singular transforms */
    d = base;
    d.object_to_world = mat4_scale(1.0f, 1e-30f, 1.0f);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);
    d = base;
    d.object_to_world = mat4_identity();
    d.object_to_world.m[0][0] = 0.0f;  /* collapses one basis direction */
    d.object_to_world.m[1][0] = 0.0f;
    d.object_to_world.m[2][0] = 0.0f;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);
    d = base;
    d.world_to_clip = mat4_identity();
    d.world_to_clip.m[0][0] = 0.0f;
    d.world_to_clip.m[1][1] = 0.0f;
    d.world_to_clip.m[2][2] = 0.0f;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);

    /* non-affine object_to_world: bottom row not (0,0,0,1) */
    d = base;
    d.object_to_world = mat4_identity();
    d.object_to_world.m[3][3] = 2.0f;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);
    d = base;
    d.object_to_world = mat4_identity();
    d.object_to_world.m[3][2] = 0.5f;
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) != 0);

    /* nothing ran, nothing changed */
    TASK_CHECK(calls == 0);
    TASK_CHECK(memcmp(color_copy, fb->color_buffer, (size_t)size) == 0);
    TASK_CHECK(memcmp(depth_copy, fb->depth_buffer,
                      (size_t)16 * 16 * sizeof(float)) == 0);

    /* uniforms may be NULL: the shader is called with NULL and still draws */
    {
        framebuffer_t *fb2 = framebuffer_create(16, 16);
        ellipsoid_draw_t dn = make_draw(&good, boom_shader, NULL, 0);
        TASK_CHECK(graphics_draw_ellipsoid(fb2, &dn) == 0);
        TASK_CHECK(pixel_channel(fb2, 8, 8, 0) == 255);
        framebuffer_release(fb2);
    }

    /* the passed matrices and configuration are not modified */
    {
        ellipsoid_draw_t copy = base;
        mat4_t obj_before = copy.object_to_world;
        mat4_t clip_before = copy.world_to_clip;
        color_uniforms_t u2;
        memset(&u2, 0, sizeof(u2));
        u2.color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
        copy = make_draw(&good, color_shader, &u2, 1);
        obj_before = copy.object_to_world;
        clip_before = copy.world_to_clip;
        TASK_CHECK(graphics_draw_ellipsoid(fb, &copy) == 0);
        TASK_CHECK(memcmp(&obj_before, &copy.object_to_world,
                          sizeof(mat4_t)) == 0);
        TASK_CHECK(memcmp(&clip_before, &copy.world_to_clip,
                          sizeof(mat4_t)) == 0);
        TASK_CHECK(copy.enable_blend == 1 && copy.shader == color_shader);
    }

    free(color_copy);
    free(depth_copy);
    framebuffer_release(fb);
    return 0;
}

/*
 * interleaved drawing and multi-frame clears: frames are reproducible and the
 * clear resets both buffers between frames.
 */
static int pipeline_interleave_and_clear(void) {
    framebuffer_t *fb = framebuffer_create(24, 24);
    ref_scene s = make_scene(mat4_identity(), mk(0, 0, 5), mk(0, 0, 0),
                             24, 24);
    color_uniforms_t sphere_u;
    ellipsoid_draw_t d;
    program_t *tri;
    ref_hit center = reference_pixel(&s, 12, 12);
    int i, x, y;

    TASK_CHECK(center.hit);
    memset(&sphere_u, 0, sizeof(sphere_u));
    sphere_u.color = vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
    d = make_draw(&s, color_shader, &sphere_u, 0);

    tri = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                         (int)sizeof(float), (int)sizeof(int), 0, 0);

    /* frame 1: triangle behind, then the sphere on top */
    clear_frame(fb);
    *(int*)program_get_uniforms(tri) = 0;
    set_triangle(tri, 0.999f);
    graphics_draw_triangle(fb, tri);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    TASK_CHECK(pixel_channel(fb, 12, 12, 0) == 255);
    TASK_CHECK(absd(pixel_depth(fb, 12, 12) - center.depth) <= 3e-4);

    /* frame 2: clear, sphere first, then a nearer triangle */
    clear_frame(fb);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    *(int*)program_get_uniforms(tri) = 0;
    set_triangle(tri, -0.98f);   /* depth ~0.01, in front */
    graphics_draw_triangle(fb, tri);
    TASK_CHECK(pixel_channel(fb, 12, 12, 2) == 255);
    TASK_CHECK(pixel_depth(fb, 12, 12) <= 0.01 + 1e-5);

    /* clear restores a pristine frame; a redraw gives the reference image */
    clear_frame(fb);
    TASK_CHECK(fb->depth_buffer[0] == 1.0f);
    TASK_CHECK(fb->color_buffer[0] == 0 && fb->color_buffer[3] == 255);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    {
        int ref_hits = 0, mismatches = 0;
        for (y = 0; y < 24; y++)
            for (x = 0; x < 24; x++) {
                ref_hit rh = reference_pixel(&s, x, y);
                if (!rh.hit) {
                    if (fb->depth_buffer[y * 24 + x] != 1.0f) mismatches++;
                    continue;
                }
                ref_hits++;
                if (pixel_channel(fb, x, y, 0) != 255) mismatches++;
                if (absd(pixel_depth(fb, x, y) - rh.depth) > 3e-4) mismatches++;
            }
        TASK_CHECK(ref_hits > 100);
        TASK_CHECK(mismatches == 0);
    }

    /* repeated frames stay stable */
    for (i = 0; i < 3; i++) {
        clear_frame(fb);
        TASK_CHECK(graphics_draw_ellipsoid(fb, &d) == 0);
    }
    TASK_CHECK(pixel_channel(fb, 12, 12, 0) == 255);
    TASK_CHECK(absd(pixel_depth(fb, 12, 12) - center.depth) <= 3e-4);

    program_release(tri);
    framebuffer_release(fb);
    return 0;
}

/*
 * the old program_t path keeps working unchanged: vertex/fragment callbacks,
 * clipping, culling, and the fragment signature are untouched.
 */
static int pipeline_triangle_compat(void) {
    framebuffer_t *fb = framebuffer_create(32, 24);
    program_t *p = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                                  (int)sizeof(float), (int)sizeof(int), 0, 0);

    *(int*)program_get_uniforms(p) = 0;
    clear_frame(fb);
    set_triangle(p, 0.0f);
    graphics_draw_triangle(fb, p);
    TASK_CHECK(pixel_channel(fb, 16, 12, 2) == 255);
    TASK_CHECK(absd(pixel_depth(fb, 16, 12) - 0.5) < 1e-6);

    /* discard through the old callback signature skips color and depth */
    clear_frame(fb);
    *(int*)program_get_uniforms(p) = 1;
    set_triangle(p, -0.5f);
    graphics_draw_triangle(fb, p);
    TASK_CHECK(pixel_channel(fb, 16, 12, 0) == 0);
    TASK_CHECK(fb->depth_buffer[12 * 32 + 16] == 1.0f);

    /* depth ordering between two triangle draws */
    clear_frame(fb);
    *(int*)program_get_uniforms(p) = 0;
    set_triangle(p, 0.5f);
    graphics_draw_triangle(fb, p);
    set_triangle(p, -0.5f);
    graphics_draw_triangle(fb, p);
    TASK_CHECK(pixel_depth(fb, 16, 12) < 0.5);
    program_release(p);

    /* blending on the old path still blends RGB and keeps alpha */
    p = program_create(tri_vs, tri_fs, (int)sizeof(tri_attrib_t),
                       (int)sizeof(float), (int)sizeof(int), 0, 1);
    framebuffer_clear_color(fb, vec4_new(1.0f, 0.0f, 0.0f, 0.7f));
    framebuffer_clear_depth(fb, 1);
    *(int*)program_get_uniforms(p) = 0;
    set_triangle(p, 0.0f);
    graphics_draw_triangle(fb, p);   /* opaque blue over opaque-ish red */
    TASK_CHECK(pixel_channel(fb, 16, 12, 2) == 255);
    TASK_CHECK(pixel_channel(fb, 16, 12, 3) == (int)(0.7f * 255));
    program_release(p);
    framebuffer_release(fb);
    return 0;
}

/* ========================================================================= */
/* registration                                                              */
/* ========================================================================= */

static const struct task_case case_list[] = {
    {"depth", "matches_reference", depth_matches_reference},
    {"depth", "root_selection", depth_root_selection},
    {"depth", "near_plane_cut", depth_near_plane_cut},
    {"depth", "triangle_occlusion", depth_triangle_occlusion},
    {"depth", "triangle_midplane", depth_triangle_midplane},

    {"surface", "nonuniform_scale", surface_nonuniform_scale},
    {"surface", "shear", surface_shear},
    {"surface", "reflection", surface_reflection},
    {"surface", "camera_inside", surface_camera_inside},

    {"pipeline", "discard", pipeline_discard},
    {"pipeline", "blend", pipeline_blend},
    {"pipeline", "equal_depth", pipeline_equal_depth},
    {"pipeline", "invalid_inputs", pipeline_invalid_inputs},
    {"pipeline", "interleave_and_clear", pipeline_interleave_and_clear},
    {"pipeline", "triangle_compat", pipeline_triangle_compat},
};

const struct task_case task_cases[] = {
    {"depth", "matches_reference", depth_matches_reference},
    {"depth", "root_selection", depth_root_selection},
    {"depth", "near_plane_cut", depth_near_plane_cut},
    {"depth", "triangle_occlusion", depth_triangle_occlusion},
    {"depth", "triangle_midplane", depth_triangle_midplane},

    {"surface", "nonuniform_scale", surface_nonuniform_scale},
    {"surface", "shear", surface_shear},
    {"surface", "reflection", surface_reflection},
    {"surface", "camera_inside", surface_camera_inside},

    {"pipeline", "discard", pipeline_discard},
    {"pipeline", "blend", pipeline_blend},
    {"pipeline", "equal_depth", pipeline_equal_depth},
    {"pipeline", "invalid_inputs", pipeline_invalid_inputs},
    {"pipeline", "interleave_and_clear", pipeline_interleave_and_clear},
    {"pipeline", "triangle_compat", pipeline_triangle_compat},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
