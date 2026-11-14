#include "task_support/check.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "renderer/core/ellipsoid.h"
#include "renderer/core/graphics.h"
#include "renderer/core/macro.h"
#include "renderer/core/maths.h"

/*
 * independent double-precision ray reference
 *
 * everything below (matrix inversion, unprojection, quadratic solve, root
 * selection, normal transform) is re-implemented here in double precision
 * and shares no code with the ellipsoid renderer under test.
 */

typedef struct {double m[4][4];} dm4_t;

static dm4_t dm4_from_mat4(mat4_t m) {
    dm4_t r;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i][j] = m.m[i][j];
        }
    }
    return r;
}

static double dm4_det3(const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
         - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
         + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

static double dm4_minor(dm4_t m, int r, int c) {
    double a[3][3];
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            a[i][j] = m.m[i < r ? i : i + 1][j < c ? j : j + 1];
        }
    }
    return dm4_det3(a);
}

static dm4_t dm4_inverse(dm4_t m) {
    dm4_t inv;
    double det = 0;
    int i, j;
    for (j = 0; j < 4; j++) {
        det += m.m[0][j] * ((j & 1) ? -1 : 1) * dm4_minor(m, 0, j);
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            inv.m[j][i] = (((i + j) & 1) ? -1 : 1) * dm4_minor(m, i, j) / det;
        }
    }
    return inv;
}

static void dm4_mul_point(dm4_t m, const double p[3], double out[3]) {
    double x = m.m[0][0] * p[0] + m.m[0][1] * p[1] + m.m[0][2] * p[2] + m.m[0][3];
    double y = m.m[1][0] * p[0] + m.m[1][1] * p[1] + m.m[1][2] * p[2] + m.m[1][3];
    double z = m.m[2][0] * p[0] + m.m[2][1] * p[1] + m.m[2][2] * p[2] + m.m[2][3];
    double w = m.m[3][0] * p[0] + m.m[3][1] * p[1] + m.m[3][2] * p[2] + m.m[3][3];
    out[0] = x / w;
    out[1] = y / w;
    out[2] = z / w;
}

typedef struct {
    int hit;
    double local[3];
    double world[3];
    double normal[3];
    double depth;
    int backface;
} ref_hit_t;

static ref_hit_t ref_cast(mat4_t o2w, mat4_t w2c, int px, int py,
                          int width, int height) {
    ref_hit_t r;
    dm4_t model = dm4_from_mat4(o2w);
    dm4_t model_inv = dm4_inverse(model);
    dm4_t proj_inv = dm4_inverse(dm4_from_mat4(w2c));
    dm4_t proj = dm4_from_mat4(w2c);
    double ndc[3];
    double near_w[3], far_w[3], o[3], f[3], d[3];
    double a, b, c, disc, root, q, t0, t1, t, tmp;
    double clip[4];
    int i;

    memset(&r, 0, sizeof(r));

    ndc[0] = ((px + 0.5) / width) * 2 - 1;
    ndc[1] = ((py + 0.5) / height) * 2 - 1;
    ndc[2] = -1;
    dm4_mul_point(proj_inv, ndc, near_w);
    ndc[2] = 1;
    dm4_mul_point(proj_inv, ndc, far_w);
    dm4_mul_point(model_inv, near_w, o);
    dm4_mul_point(model_inv, far_w, f);
    for (i = 0; i < 3; i++) {
        d[i] = f[i] - o[i];
    }

    a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    b = 2 * (o[0] * d[0] + o[1] * d[1] + o[2] * d[2]);
    c = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1;
    if (!(a > 0)) {
        return r;
    }
    disc = b * b - 4 * a * c;
    if (!(disc >= 0)) {
        return r;
    }
    root = sqrt(disc);
    q = -0.5 * (b + (b >= 0 ? root : -root));
    if (q != 0) {
        t0 = q / a;
        t1 = c / q;
    } else {
        t0 = t1 = 0;
    }
    if (t0 > t1) {
        tmp = t0;
        t0 = t1;
        t1 = tmp;
    }
    if (t0 >= 0 && t0 <= 1) {
        t = t0;
    } else if (t1 >= 0 && t1 <= 1) {
        t = t1;
    } else {
        return r;
    }

    r.hit = 1;
    for (i = 0; i < 3; i++) {
        r.local[i] = o[i] + t * d[i];
    }
    dm4_mul_point(model, r.local, r.world);
    /* world normal: inverse-transpose of the linear part, normalized */
    {
        double n[3], len;
        for (i = 0; i < 3; i++) {
            n[i] = model_inv.m[0][i] * r.local[0]
                 + model_inv.m[1][i] * r.local[1]
                 + model_inv.m[2][i] * r.local[2];
        }
        len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (i = 0; i < 3; i++) {
            r.normal[i] = n[i] / len;
        }
    }
    for (i = 0; i < 4; i++) {
        clip[i] = proj.m[i][0] * r.world[0] + proj.m[i][1] * r.world[1]
                + proj.m[i][2] * r.world[2] + proj.m[i][3];
    }
    r.depth = (clip[2] / clip[3] + 1) * 0.5;
    {
        double view[3], dot = 0;
        for (i = 0; i < 3; i++) {
            view[i] = far_w[i] - near_w[i];
            dot += r.normal[i] * view[i];
        }
        r.backface = dot > 0 ? 1 : 0;
    }
    return r;
}

static int ref_hit_count(mat4_t o2w, mat4_t w2c, int width, int height) {
    int x, y, count = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            if (ref_cast(o2w, w2c, x, y, width, height).hit) {
                count++;
            }
        }
    }
    return count;
}

/* shared projections used by the cases */

static mat4_t proj_ortho(void) {
    return mat4_ortho(-2, 2, -2, 2, 1, 5);
}

/* capturing ellipsoid shader */

#define MAX_HITS 4096

typedef struct {
    ellipsoid_hit_t hits[MAX_HITS];
    int count;
    vec4_t color;
    int discard;
} capture_t;

static capture_t g_cap;

static void reset_capture(vec4_t color) {
    g_cap.count = 0;
    g_cap.color = color;
    g_cap.discard = 0;
}

static vec4_t capture_shader(const ellipsoid_hit_t *hit, void *uniforms,
                             int *discard) {
    capture_t *cap = (capture_t*)uniforms;
    if (cap->count < MAX_HITS) {
        cap->hits[cap->count] = *hit;
    }
    cap->count++;
    if (cap->discard) {
        *discard = 1;
    }
    return cap->color;
}

static vec4_t solid_shader(const ellipsoid_hit_t *hit, void *uniforms,
                           int *discard) {
    UNUSED_VAR(hit);
    UNUSED_VAR(discard);
    return *(vec4_t*)uniforms;
}

static int g_saw_null_uniforms;

static vec4_t probe_shader(const ellipsoid_hit_t *hit, void *uniforms,
                           int *discard) {
    UNUSED_VAR(hit);
    UNUSED_VAR(discard);
    if (uniforms == NULL) {
        g_saw_null_uniforms = 1;
    }
    return vec4_new(0, 1, 0, 1);
}

static ellipsoid_draw_t make_draw(mat4_t o2w, mat4_t w2c,
                                  ellipsoid_shader_t *shader, void *uniforms,
                                  int blend) {
    ellipsoid_draw_t draw;
    draw.object_to_world = o2w;
    draw.world_to_clip = w2c;
    draw.shader = shader;
    draw.uniforms = uniforms;
    draw.enable_blend = blend;
    return draw;
}

static const ellipsoid_hit_t *find_hit(const double world[3], double tol) {
    int i;
    for (i = 0; i < g_cap.count && i < MAX_HITS; i++) {
        const ellipsoid_hit_t *h = &g_cap.hits[i];
        double dx = h->world_position.x - world[0];
        double dy = h->world_position.y - world[1];
        double dz = h->world_position.z - world[2];
        if (dx * dx + dy * dy + dz * dz < tol * tol) {
            return h;
        }
    }
    return NULL;
}

static int vec3_close(vec3_t v, const double ref[3], double tol) {
    return fabs(v.x - ref[0]) <= tol
        && fabs(v.y - ref[1]) <= tol
        && fabs(v.z - ref[2]) <= tol;
}

static int check_hit(const ellipsoid_hit_t *hit, const ref_hit_t *ref) {
    TASK_CHECK(vec3_close(hit->local_position, ref->local, 1e-4));
    TASK_CHECK(vec3_close(hit->world_position, ref->world, 1e-4));
    TASK_CHECK(vec3_close(hit->world_normal, ref->normal, 1e-3));
    TASK_CHECK(fabs(hit->depth - ref->depth) < 1e-4);
    TASK_CHECK(hit->backface == ref->backface);
    TASK_CHECK(fabs(vec3_length(hit->local_position) - 1) < 1e-4);
    TASK_CHECK(fabs(vec3_length(hit->world_normal) - 1) < 1e-4);
    return 0;
}

static int check_surface_pixels(mat4_t o2w, mat4_t w2c,
                                const int *pixels, int n,
                                int width, int height) {
    int i, k;
    for (i = 0; i < n; i++) {
        ref_hit_t ref = ref_cast(o2w, w2c, pixels[2 * i], pixels[2 * i + 1],
                                 width, height);
        const ellipsoid_hit_t *hit;
        TASK_CHECK(ref.hit);
        hit = find_hit(ref.world, 1e-3);
        TASK_CHECK(hit != NULL);
        TASK_CHECK(check_hit(hit, &ref) == 0);
    }
    for (k = 0; k < g_cap.count && k < MAX_HITS; k++) {
        TASK_CHECK(fabs(vec3_length(g_cap.hits[k].local_position) - 1) < 1e-3);
        TASK_CHECK(fabs(vec3_length(g_cap.hits[k].world_normal) - 1) < 1e-3);
    }
    return 0;
}

/* legacy triangle program, exercising the pre-existing pipeline */

typedef struct {vec4_t position;} legacy_attribs_t;

static vec4_t legacy_vs(void *attribs, void *varyings, void *uniforms) {
    UNUSED_VAR(varyings);
    UNUSED_VAR(uniforms);
    return ((legacy_attribs_t*)attribs)->position;
}

static vec4_t legacy_fs(void *varyings, void *uniforms, int *discard,
                        int backface) {
    UNUSED_VAR(varyings);
    UNUSED_VAR(discard);
    UNUSED_VAR(backface);
    return *(vec4_t*)uniforms;
}

static program_t *legacy_program(void) {
    return program_create(legacy_vs, legacy_fs, sizeof(legacy_attribs_t),
                          sizeof(float), sizeof(vec4_t), 1, 0);
}

static void draw_screen_triangle(framebuffer_t *fb, program_t *prog,
                                 float ndc_z, vec4_t color) {
    legacy_attribs_t *attribs;
    *(vec4_t*)program_get_uniforms(prog) = color;
    attribs = (legacy_attribs_t*)program_get_attribs(prog, 0);
    attribs->position = vec4_new(-1, -1, ndc_z, 1);
    attribs = (legacy_attribs_t*)program_get_attribs(prog, 1);
    attribs->position = vec4_new(3, -1, ndc_z, 1);
    attribs = (legacy_attribs_t*)program_get_attribs(prog, 2);
    attribs->position = vec4_new(-1, 3, ndc_z, 1);
    graphics_draw_triangle(fb, prog);
}

/* depth group */

static int case_front_root(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    ref_hit_t ref;
    const ellipsoid_hit_t *hit;

    reset_capture(vec4_new(1, 0, 0, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);

    /* the shader ran exactly once per pixel whose ray hits the surface */
    TASK_CHECK(g_cap.count > 0);
    TASK_CHECK(g_cap.count == ref_hit_count(o2w, w2c, 16, 16));

    /* the center pixel sees the front (near) root, not the back one */
    ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    TASK_CHECK(ref.hit);
    TASK_CHECK(ref.backface == 0);
    TASK_CHECK(ref.depth < 0.5);
    hit = find_hit(ref.world, 1e-3);
    TASK_CHECK(hit != NULL);
    TASK_CHECK(check_hit(hit, &ref) == 0);
    TASK_CHECK(fabs(fb->depth_buffer[8 * 16 + 8] - ref.depth) < 1e-4);

    /* a corner pixel misses the silhouette and stays untouched */
    TASK_CHECK(fb->depth_buffer[0] == 1.0f);
    TASK_CHECK(fb->color_buffer[0] == 0);

    framebuffer_release(fb);
    return 0;
}

static int case_near_plane_cut(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_identity();
    mat4_t w2c = mat4_ortho(-2, 2, -2, 2, 0.5f, 5);
    ellipsoid_draw_t draw;
    ref_hit_t ref;
    const ellipsoid_hit_t *hit;

    reset_capture(vec4_new(1, 0, 0, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(g_cap.count == ref_hit_count(o2w, w2c, 16, 16));

    /* the near plane slices the unit sphere: the exit root is used */
    ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    TASK_CHECK(ref.hit);
    TASK_CHECK(ref.backface == 1);
    hit = find_hit(ref.world, 1e-3);
    TASK_CHECK(hit != NULL);
    TASK_CHECK(check_hit(hit, &ref) == 0);
    /* the exit point is on the far hemisphere, not a near-plane cap */
    TASK_CHECK(hit->world_position.z < -0.9f);
    TASK_CHECK(hit->depth > 0.01f);
    TASK_CHECK(fabs(fb->depth_buffer[8 * 16 + 8] - ref.depth) < 1e-4);

    framebuffer_release(fb);
    return 0;
}

static int case_perspective_inside(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_scale(2, 2, 2);
    mat4_t w2c = mat4_perspective(TO_RADIANS(60), 1, 0.1f, 10);
    ellipsoid_draw_t draw;
    ref_hit_t ref;
    const ellipsoid_hit_t *hit;

    reset_capture(vec4_new(1, 0, 0, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);

    /* the camera sits inside the sphere: every pixel hits the exit wall */
    TASK_CHECK(g_cap.count == 16 * 16);
    ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    TASK_CHECK(ref.hit);
    TASK_CHECK(ref.backface == 1);
    hit = find_hit(ref.world, 1e-3);
    TASK_CHECK(hit != NULL);
    TASK_CHECK(check_hit(hit, &ref) == 0);
    TASK_CHECK(fabs(fb->depth_buffer[8 * 16 + 8] - ref.depth) < 1e-4);

    framebuffer_release(fb);
    return 0;
}

static int case_triangle_occlusion(void) {
    framebuffer_t *fb = framebuffer_create(32, 32);
    program_t *prog = legacy_program();
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    vec4_t red = vec4_new(1, 0, 0, 1);
    vec4_t green = vec4_new(0, 1, 0, 1);
    vec4_t blue = vec4_new(0, 0, 1, 1);
    ellipsoid_draw_t draw = make_draw(o2w, w2c, solid_shader, &red, 0);
    ref_hit_t ref = ref_cast(o2w, w2c, 16, 16, 32, 32);
    int center = 16 * 32 + 16;
    int corner = 2 * 32 + 2;

    TASK_CHECK(ref.hit);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 255);

    /* a farther triangle must not overwrite the ellipsoid fragment */
    draw_screen_triangle(fb, prog, -0.25f, green);  /* depth 0.375 */
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 255);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 0);
    TASK_CHECK(fabs(fb->depth_buffer[center] - ref.depth) < 1e-4);

    /* a nearer triangle overwrites it through the shared depth buffer */
    draw_screen_triangle(fb, prog, -0.75f, blue);  /* depth 0.125 */
    TASK_CHECK(fb->color_buffer[center * 4 + 2] == 255);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 0);
    TASK_CHECK(fabs(fb->depth_buffer[center] - 0.125f) < 1e-5);

    /* outside the silhouette only the triangles wrote */
    TASK_CHECK(fb->color_buffer[corner * 4 + 2] == 255);
    TASK_CHECK(fabs(fb->depth_buffer[corner] - 0.125f) < 1e-5);

    program_release(prog);
    framebuffer_release(fb);
    return 0;
}

/* surface group */

static int case_nonuniform_scale(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_mul_mat4(mat4_translate(0.3f, -0.2f, -3),
                               mat4_scale(1.5f, 0.8f, 0.6f));
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    int pixels[] = {8, 8, 5, 9, 10, 4, 7, 7};

    reset_capture(vec4_new(1, 1, 1, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(g_cap.count == ref_hit_count(o2w, w2c, 16, 16));
    TASK_CHECK(check_surface_pixels(o2w, w2c, pixels, 4, 16, 16) == 0);

    framebuffer_release(fb);
    return 0;
}

static int case_shear(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_identity();
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    int pixels[] = {8, 8, 9, 8, 8, 9, 7, 7};

    o2w.m[0][1] = 0.6f;
    o2w.m[0][2] = -0.3f;
    o2w.m[1][2] = 0.4f;
    o2w.m[0][3] = 0.1f;
    o2w.m[1][3] = 0.2f;
    o2w.m[2][3] = -3.0f;

    reset_capture(vec4_new(1, 1, 1, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(g_cap.count == ref_hit_count(o2w, w2c, 16, 16));
    TASK_CHECK(check_surface_pixels(o2w, w2c, pixels, 4, 16, 16) == 0);

    framebuffer_release(fb);
    return 0;
}

static int case_reflection(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_mul_mat4(mat4_translate(0.1f, 0, -3),
                               mat4_scale(-1.2f, 0.9f, 0.7f));
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    vec3_t center = vec3_new(0.1f, 0, -3);
    int pixels[] = {8, 8, 6, 9, 10, 6, 7, 8};
    int k;

    reset_capture(vec4_new(1, 1, 1, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(g_cap.count == ref_hit_count(o2w, w2c, 16, 16));
    TASK_CHECK(check_surface_pixels(o2w, w2c, pixels, 4, 16, 16) == 0);

    /* reflection must not flip the outward normal convention */
    for (k = 0; k < g_cap.count && k < MAX_HITS; k++) {
        vec3_t to_hit = vec3_sub(g_cap.hits[k].world_position, center);
        TASK_CHECK(vec3_dot(g_cap.hits[k].world_normal, to_hit) > 0);
    }

    framebuffer_release(fb);
    return 0;
}

/* pipeline group */

static int case_discard(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    int i;

    reset_capture(vec4_new(1, 0, 0, 1));
    g_cap.discard = 1;
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);

    /* the shader ran, but neither color nor depth may change */
    TASK_CHECK(g_cap.count > 0);
    for (i = 0; i < 16 * 16; i++) {
        TASK_CHECK(fb->depth_buffer[i] == 1.0f);
        TASK_CHECK(fb->color_buffer[i * 4 + 0] == 0);
        TASK_CHECK(fb->color_buffer[i * 4 + 1] == 0);
        TASK_CHECK(fb->color_buffer[i * 4 + 2] == 0);
        TASK_CHECK(fb->color_buffer[i * 4 + 3] == 255);
    }

    framebuffer_release(fb);
    return 0;
}

static int case_blend(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    ellipsoid_draw_t draw;
    ref_hit_t ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    unsigned char expect_r, expect_g, expect_b;
    int center = 8 * 16 + 8;

    /* blended: src * src_alpha + dst * (1 - src_alpha), alpha untouched */
    framebuffer_clear_color(fb, vec4_new(0.2f, 0.4f, 0.6f, 1));
    reset_capture(vec4_new(1, 0, 0, 0.5f));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 1);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    expect_r = float_to_uchar(1 * 0.5f + float_from_uchar(float_to_uchar(0.2f)) * 0.5f);
    expect_g = float_to_uchar(0 * 0.5f + float_from_uchar(float_to_uchar(0.4f)) * 0.5f);
    expect_b = float_to_uchar(0 * 0.5f + float_from_uchar(float_to_uchar(0.6f)) * 0.5f);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == expect_r);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == expect_g);
    TASK_CHECK(fb->color_buffer[center * 4 + 2] == expect_b);
    TASK_CHECK(fb->color_buffer[center * 4 + 3] == 255);
    TASK_CHECK(fabs(fb->depth_buffer[center] - ref.depth) < 1e-4);

    /* unblended: the shader output is saturated before writing */
    framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(fb, 1);
    reset_capture(vec4_new(2, -1, 0.5f, 1));
    draw = make_draw(o2w, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 255);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 2] == float_to_uchar(0.5f));
    TASK_CHECK(fb->color_buffer[center * 4 + 3] == 255);

    framebuffer_release(fb);
    return 0;
}

static int case_equal_depth(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    vec4_t red = vec4_new(1, 0, 0, 1);
    vec4_t green = vec4_new(0, 1, 0, 1);
    ellipsoid_draw_t draw_red = make_draw(o2w, w2c, solid_shader, &red, 0);
    ellipsoid_draw_t draw_green = make_draw(o2w, w2c, solid_shader, &green, 0);
    ref_hit_t ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    int center = 8 * 16 + 8;

    /* identical depth passes the shared <= comparison: last draw wins */
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw_red) == 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw_green) == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 255);
    TASK_CHECK(fabs(fb->depth_buffer[center] - ref.depth) < 1e-4);

    framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(fb, 1);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw_green) == 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw_red) == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 0] == 255);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 0);

    framebuffer_release(fb);
    return 0;
}

static int case_invalid_inputs(void) {
    framebuffer_t *fb = framebuffer_create(8, 8);
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    unsigned char color_snapshot[8 * 8 * 4];
    float depth_snapshot[8 * 8];
    ellipsoid_draw_t draw;
    mat4_t bad;

    memcpy(color_snapshot, fb->color_buffer, sizeof(color_snapshot));
    memcpy(depth_snapshot, fb->depth_buffer, sizeof(depth_snapshot));

    /* non-finite object_to_world */
    reset_capture(vec4_new(1, 0, 0, 1));
    bad = o2w;
    bad.m[0][0] = NAN;
    draw = make_draw(bad, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) != 0);

    /* non-finite world_to_clip */
    bad = w2c;
    bad.m[2][3] = INFINITY;
    draw = make_draw(o2w, bad, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) != 0);

    /* singular object_to_world */
    bad = mat4_identity();
    bad.m[1][1] = 0;
    draw = make_draw(bad, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) != 0);

    /* non-affine object_to_world */
    bad = o2w;
    bad.m[3][2] = -1;
    draw = make_draw(bad, w2c, capture_shader, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) != 0);

    /* missing shader */
    draw = make_draw(o2w, w2c, NULL, &g_cap, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) != 0);

    /* nothing was shaded or written by any of the rejected draws */
    TASK_CHECK(g_cap.count == 0);
    TASK_CHECK(memcmp(fb->color_buffer, color_snapshot,
                      sizeof(color_snapshot)) == 0);
    TASK_CHECK(memcmp(fb->depth_buffer, depth_snapshot,
                      sizeof(depth_snapshot)) == 0);

    /* the valid configuration still succeeds, with NULL uniforms allowed */
    g_saw_null_uniforms = 0;
    draw = make_draw(o2w, w2c, probe_shader, NULL, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(g_saw_null_uniforms);

    framebuffer_release(fb);
    return 0;
}

static int case_clear_and_legacy(void) {
    framebuffer_t *fb = framebuffer_create(16, 16);
    program_t *prog = legacy_program();
    mat4_t o2w = mat4_translate(0, 0, -3);
    mat4_t w2c = proj_ortho();
    vec4_t red = vec4_new(1, 0, 0, 1);
    vec4_t green = vec4_new(0, 1, 0, 1);
    vec4_t blue = vec4_new(0, 0, 1, 1);
    ellipsoid_draw_t draw;
    ref_hit_t ref = ref_cast(o2w, w2c, 8, 8, 16, 16);
    int center = 8 * 16 + 8;

    /* frame 1: ellipsoid, then clear, then a legacy triangle */
    draw = make_draw(o2w, w2c, solid_shader, &red, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(fb, 1);
    TASK_CHECK(fb->depth_buffer[center] == 1.0f);
    draw_screen_triangle(fb, prog, 0, green);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 255);
    TASK_CHECK(fabs(fb->depth_buffer[center] - 0.5f) < 1e-5);

    /* frame 1 continued: the ellipsoid wins against the triangle depth */
    draw = make_draw(o2w, w2c, solid_shader, &blue, 0);
    TASK_CHECK(graphics_draw_ellipsoid(fb, &draw) == 0);
    TASK_CHECK(fb->color_buffer[center * 4 + 2] == 255);
    TASK_CHECK(fabs(fb->depth_buffer[center] - ref.depth) < 1e-4);

    /* frame 2: after another clear the legacy path still works alone */
    framebuffer_clear_color(fb, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(fb, 1);
    draw_screen_triangle(fb, prog, 0, green);
    TASK_CHECK(fb->color_buffer[center * 4 + 1] == 255);
    TASK_CHECK(fb->color_buffer[center * 4 + 2] == 0);
    TASK_CHECK(fabs(fb->depth_buffer[center] - 0.5f) < 1e-5);

    program_release(prog);
    framebuffer_release(fb);
    return 0;
}

/* case table */

const struct task_case task_cases[] = {
    {"depth", "front_root", case_front_root},
    {"depth", "near_plane_cut", case_near_plane_cut},
    {"depth", "perspective_inside", case_perspective_inside},
    {"depth", "triangle_occlusion", case_triangle_occlusion},
    {"surface", "nonuniform_scale", case_nonuniform_scale},
    {"surface", "shear", case_shear},
    {"surface", "reflection", case_reflection},
    {"pipeline", "discard", case_discard},
    {"pipeline", "blend", case_blend},
    {"pipeline", "equal_depth", case_equal_depth},
    {"pipeline", "invalid_inputs", case_invalid_inputs},
    {"pipeline", "clear_and_legacy", case_clear_and_legacy},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
