#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "gl.h"
#include "mat.h"
#include "probe.h"

#define PROBE_PI 3.14159265358979323846f

/********************************************************************
 *                                                                  *
 *                        small vector helpers                      *
 *                                                                  *
 ********************************************************************/

static float
p_dot(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void
p_cross(float *out, const float *a, const float *b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float
p_len(const float *a)
{
    return sqrtf(p_dot(a, a));
}

static void
p_normalize(float *a)
{
    float m = p_len(a);
    if (m > 0) {
        a[0] /= m;
        a[1] /= m;
        a[2] /= m;
    }
}

static int
p_finite3(const float *a)
{
    return isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]);
}

static int
clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/********************************************************************
 *                                                                  *
 *                          sh basis helpers                        *
 *                                                                  *
 ********************************************************************/

/* nine real sh basis functions for a unit direction */
static void
sh_basis(const float dir[3], float basis[9])
{
    float x = dir[0];
    float y = dir[1];
    float z = dir[2];

    basis[0] = 0.28209479177387814f;
    basis[1] = 0.4886025119029199f * y;
    basis[2] = 0.4886025119029199f * z;
    basis[3] = 0.4886025119029199f * x;
    basis[4] = 1.0925484305920792f * x * y;
    basis[5] = 1.0925484305920792f * y * z;
    basis[6] = 0.31539156525252005f * (2 * z * z - x * x - y * y);
    basis[7] = 1.0925484305920792f * x * z;
    basis[8] = 0.5462742152960396f * (x * x - y * y);
}

/* lambert convolution of radiance coefficients into irradiance */
static void
sh_lambert(const float radiance[9], float irradiance[9])
{
    static const float a0 = PROBE_PI;
    static const float a1 = 2.0943951023931953f;
    static const float a2 = 0.7853981633974483f;

    irradiance[0] = radiance[0] * a0;
    for (int i = 1; i < 4; i++)
        irradiance[i] = radiance[i] * a1;
    for (int i = 4; i < 9; i++)
        irradiance[i] = radiance[i] * a2;
}

/* reconstructs one channel without clipping negative lobes */
static float
sh_reconstruct(const float irradiance[9], const float basis[9])
{
    float e = 0;
    for (int i = 0; i < 9; i++)
        e += irradiance[i] * basis[i];
    return e;
}

/********************************************************************
 *                                                                  *
 *                        cube face geometry                        *
 *                                                                  *
 ********************************************************************/

/* forward/up per face in the order +X, -X, +Y, -Y, +Z, -Z */

void
probe_face_axes(int face, float forward[3], float up[3], float right[3])
{
    static const float table[6][2][3] = {
        {{ 1, 0, 0}, { 0, -1, 0}},
        {{-1, 0, 0}, { 0, -1, 0}},
        {{ 0, 1, 0}, { 0,  0, 1}},
        {{ 0,-1, 0}, { 0,  0,-1}},
        {{ 0, 0, 1}, { 0, -1, 0}},
        {{ 0, 0,-1}, { 0, -1, 0}},
    };
    if (face < 0 || face > 5) {
        forward[0] = forward[1] = forward[2] = 0;
        up[0] = up[1] = up[2] = 0;
        right[0] = right[1] = right[2] = 0;
        return;
    }
    memcpy(forward, table[face][0], 3 * sizeof(float));
    memcpy(up, table[face][1], 3 * sizeof(float));
    p_cross(right, forward, up);
}

/* unit direction through the centre of texel (x, y) of an n x n face */

void
probe_texel_direction(int face, int x, int y, int n, float direction[3])
{
    float forward[3], up[3], right[3];
    float u = 2.0f * ((float)x + 0.5f) / (float)n - 1.0f;
    float v = 1.0f - 2.0f * ((float)y + 0.5f) / (float)n;

    probe_face_axes(face, forward, up, right);

    direction[0] = forward[0] + u * right[0] + v * up[0];
    direction[1] = forward[1] + u * right[1] + v * up[1];
    direction[2] = forward[2] + u * right[2] + v * up[2];
    p_normalize(direction);
}

/* directions of the four corners of texel (x, y), going around it */
static void
corner_dirs(int face, int x, int y, int n, float out[4][3])
{
    static const int cx[4] = {0, 1, 1, 0};
    static const int cy[4] = {0, 0, 1, 1};
    float forward[3], up[3], right[3];

    probe_face_axes(face, forward, up, right);

    for (int i = 0; i < 4; i++) {
        float u = 2.0f * ((float)(x + cx[i])) / (float)n - 1.0f;
        float v = 1.0f - 2.0f * ((float)(y + cy[i])) / (float)n;
        out[i][0] = forward[0] + u * right[0] + v * up[0];
        out[i][1] = forward[1] + u * right[1] + v * up[1];
        out[i][2] = forward[2] + u * right[2] + v * up[2];
        p_normalize(out[i]);
    }
}

/* solid angle of the spherical triangle through three unit vectors */
static float
tri_solid_angle(const float *a, const float *b, const float *c)
{
    float bc[3];
    float numerator, denominator;

    p_cross(bc, b, c);
    numerator = fabsf(p_dot(a, bc));
    denominator = 1.0f + p_dot(a, b) + p_dot(b, c) + p_dot(c, a);

    return 2.0f * atan2f(numerator, denominator);
}

/* solid angle subtended by one texel, integrated over its four corners */

float
probe_texel_solid_angle(int face, int x, int y, int n)
{
    float c[4][3];
    if (face < 0 || face > 5 || n <= 0 || x < 0 || y < 0 || x >= n || y >= n)
        return 0;
    corner_dirs(face, x, y, n, c);
    return tri_solid_angle(c[0], c[1], c[2])
         + tri_solid_angle(c[0], c[2], c[3]);
}

/********************************************************************
 *                                                                  *
 *                       private data structures                    *
 *                                                                  *
 ********************************************************************/

/* one cube-face sample: five interleaved floats per texel */
struct probe_texel {
    float radiance[3];
    float distance;
    float distance_squared;
};

struct probe_generation {
    struct probe_texel *faces;  /* probes * 6 * face_size^2 */
    float *irradiance;          /* probes * 27, channel-major bands */
};

struct probe_set {
    struct probe_config config;

    /* frozen copies of the source data */
    struct probe_triangle *triangles;
    int triangle_count;

    /* expanded capture geometry, three vertices per triangle */
    float *capture_pts;       /* capture_vertex_count * 10 */
    int *capture_indices;     /* capture_vertex_count */
    int capture_vertex_count;

    /* per-texel solid angle, shared by all probes */
    float *solid_angle;       /* 6 * face_size^2 */

    /* scratch framebuffer for a single face */
    uint32_t *scratch_colors;
    float *scratch_depths;

    /* committed and pending generations */
    struct probe_generation current;
    struct probe_generation pending;
    int has_current;

    /* pending progress */
    int pending_active;
    int pending_probe;
    int pending_face;
    float environment[3];

    struct gl_probe_volume *volume;
};

/* the phong-facing field, aliases a probe set */
struct gl_probe_volume {
    struct probe_set *set;
    float bias;
};

static int
probe_count(const struct probe_set *set)
{
    return set->config.nx * set->config.ny * set->config.nz;
}

static int
face_texel_count(const struct probe_set *set)
{
    return set->config.face_size * set->config.face_size;
}

static void
probe_position(const struct probe_set *set, int index, float pos[3])
{
    int nx = set->config.nx;
    int ny = set->config.ny;
    int x = index % nx;
    int y = (index / nx) % ny;
    int z = index / (nx * ny);

    pos[0] = set->config.origin[0] + (float)x * set->config.spacing;
    pos[1] = set->config.origin[1] + (float)y * set->config.spacing;
    pos[2] = set->config.origin[2] + (float)z * set->config.spacing;
}

/* face image of one probe inside a generation */
static struct probe_texel *
generation_face(struct probe_generation *gen, const struct probe_set *set,
                int probe, int face)
{
    size_t texels = (size_t)face_texel_count(set);
    size_t offset = ((size_t)probe * 6 + (size_t)face) * texels;
    return gen->faces + offset;
}

/********************************************************************
 *                                                                  *
 *                          capture shaders                         *
 *                                                                  *
 ********************************************************************/

/**
 * in (10):
 *    float px, py, pz;               world position
 *    float nx, ny, nz, d;            world-space triangle plane
 *    float r, g, b;                  emitted radiance
 *
 * out (10):
 *    float x, y, z, w;               clip position
 *    float wx, wy, wz;               perspective-correct world position
 *    float r, g, b;                  constant per triangle radiance
 *
 * constant attributes survive the rasterizer's perspective divide
 * unchanged, so the depth observer reads the exact emitted radiance of
 * the triangle that won and the true world position of the hit.
 */

static void
capture_vs(float *out, float *in, void *uniform)
{
    struct mat4 *mvp = (struct mat4 *)uniform;
    float tmp[4] = { in[0], in[1], in[2], 1 };

    vec4_matmul(out, mvp, tmp);

    out[4] = in[0];
    out[5] = in[1];
    out[6] = in[2];
    out[7] = in[7];
    out[8] = in[8];
    out[9] = in[9];
}

static void
capture_fs(uint32_t *out, float *in, void *uniform)
{
    (void)uniform;
    (void)in;
    *out = 0;   /* radiance is kept in float by the depth observer */
}

struct capture_context {
    struct probe_set *set;
    struct probe_texel *image;
    float position[3];
    int face;
};

/* observer run only for fragments that won the depth test */

static void
capture_fragment(void *user, int x, int y, float *frag)
{
    struct capture_context *ctx = (struct capture_context *)user;
    struct probe_set *set = ctx->set;
    int size = set->config.face_size;
    float to_p[3];
    float t;
    struct probe_texel *texel;

    if (x < 0 || x >= size || y < 0 || y >= size)
        return;

    /* the rasterizer hands back the perspective-correct world position
     * of the winning surface, so the radial distance is exact and does
     * not have to be inferred from the depth value */
    to_p[0] = frag[4] - ctx->position[0];
    to_p[1] = frag[5] - ctx->position[1];
    to_p[2] = frag[6] - ctx->position[2];
    t = p_len(to_p);
    if (!(t > 0) || !isfinite(t))
        return;
    /* geometry clipped exactly at the far plane rounds to the sentinel;
     * treat it as a miss rather than a surface sample */
    if (t >= set->config.far_plane)
        return;

    /* the emitted radiance is constant per triangle and survives the
     * rasterizer's perspective divide unchanged */
    texel = ctx->image + (size_t)y * (size_t)size + (size_t)x;
    texel->radiance[0] = frag[7];
    texel->radiance[1] = frag[8];
    texel->radiance[2] = frag[9];
    texel->distance = t;
    texel->distance_squared = t * t;
}

/********************************************************************
 *                                                                  *
 *                       capture face rendering                     *
 *                                                                  *
 ********************************************************************/

/* view + projection for one face, 90 degree square frustum.
 * the camera looks down -Z in eye space, matching gl_perspective */

static void
build_face_matrix(const float position[3], int face, float near_plane,
                  float far_plane, struct mat4 *m)
{
    float forward[3], up[3], right[3];
    float a = (far_plane + near_plane) / (near_plane - far_plane);
    float b = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    float pf;

    probe_face_axes(face, forward, up, right);
    pf = p_dot(forward, position);

    memset(m, 0, sizeof(*m));

    m->e00 = right[0];
    m->e01 = right[1];
    m->e02 = right[2];
    m->e03 = -p_dot(right, position);

    m->e10 = up[0];
    m->e11 = up[1];
    m->e12 = up[2];
    m->e13 = -p_dot(up, position);

    /* eye z is -forward . (p - position) */
    m->e20 = -a * forward[0];
    m->e21 = -a * forward[1];
    m->e22 = -a * forward[2];
    m->e23 = a * pf + b;

    m->e30 = forward[0];
    m->e31 = forward[1];
    m->e32 = forward[2];
    m->e33 = -pf;
}

static void
capture_face(struct probe_set *set, int probe, int face,
             struct probe_generation *gen)
{
    struct probe_texel *image = generation_face(gen, set, probe, face);
    int size = set->config.face_size;
    int texels = size * size;
    float position[3];
    struct mat4 matrix;
    struct capture_context ctx;
    struct gl_framebuffer fbuf;
    struct gl_pipeline pipe;

    probe_position(set, probe, position);
    build_face_matrix(position, face, set->config.near_plane,
                      set->config.far_plane, &matrix);

    /* misses carry the caller's constant environment at the far plane */
    for (int i = 0; i < texels; i++) {
        image[i].radiance[0] = set->environment[0];
        image[i].radiance[1] = set->environment[1];
        image[i].radiance[2] = set->environment[2];
        image[i].distance = set->config.far_plane;
        image[i].distance_squared =
            set->config.far_plane * set->config.far_plane;
    }
    memset(set->scratch_colors, 0, (size_t)texels * sizeof(uint32_t));
    /* this rasterizer stores the perspective-correct view-axis
     * distance in the depth buffer, so a face far plane is the
     * natural empty value: only surfaces inside the face frustum win */
    for (int i = 0; i < texels; i++)
        set->scratch_depths[i] = set->config.far_plane;

    ctx.set = set;
    ctx.image = image;
    ctx.face = face;
    ctx.position[0] = position[0];
    ctx.position[1] = position[1];
    ctx.position[2] = position[2];

    fbuf.colors = set->scratch_colors;
    fbuf.depths = set->scratch_depths;
    fbuf.width = size;
    fbuf.height = size;

    memset(&pipe, 0, sizeof(pipe));
    pipe.fbuf = &fbuf;
    pipe.uniform = &matrix;
    pipe.vs = capture_vs;
    pipe.fs = capture_fs;
    pipe.pts_in = set->capture_pts;
    pipe.n_pts = set->capture_vertex_count;
    pipe.n_attr_in = 10;
    pipe.n_attr_out = 10;
    pipe.depth_frag = capture_fragment;
    pipe.depth_user = &ctx;

    if (set->capture_vertex_count > 0) {
        /* geometry is two sided: render each winding once */
        pipe.winding = GL_WINDING_ORDER_CCW;
        gl_render(&pipe, set->capture_indices, set->capture_vertex_count,
                  GL_TRIANGLE_LIST);
        pipe.winding = GL_WINDING_ORDER_CW;
        gl_render(&pipe, set->capture_indices, set->capture_vertex_count,
                  GL_TRIANGLE_LIST);
    }
}

/********************************************************************
 *                                                                  *
 *                    sh projection of one face                     *
 *                                                                  *
 ********************************************************************/

static void
project_face(const struct probe_set *set, const struct probe_texel *image,
             int face, float *radiance_coeff)
{
    int size = set->config.face_size;
    int texels = size * size;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int i = y * size + x;
            float dir[3];
            float basis[9];
            float omega = set->solid_angle[(size_t)face * texels + i];

            probe_texel_direction(face, x, y, size, dir);
            sh_basis(dir, basis);

            for (int c = 0; c < 3; c++) {
                float lw = image[i].radiance[c] * omega;
                for (int k = 0; k < 9; k++)
                    radiance_coeff[c * 9 + k] += lw * basis[k];
            }
        }
    }
}

/********************************************************************
 *                                                                  *
 *                       distance moment lookup                     *
 *                                                                  *
 ********************************************************************/

/* picks the cube face whose axis dominates a direction */
static int
dominant_face(const float dir[3])
{
    float ax = fabsf(dir[0]);
    float ay = fabsf(dir[1]);
    float az = fabsf(dir[2]);

    if (ax >= ay && ax >= az)
        return dir[0] >= 0 ? 0 : 1;
    if (ay >= az)
        return dir[1] >= 0 ? 2 : 3;
    return dir[2] >= 0 ? 4 : 5;
}

/* unclamped face coordinates of a direction already on that face */
static void
face_coords(const float dir[3], int face, float *u, float *v)
{
    float forward[3], up[3], right[3];
    float denom;

    probe_face_axes(face, forward, up, right);
    denom = p_dot(dir, forward);
    if (fabsf(denom) < 1e-20f) {
        *u = 0;
        *v = 0;
        return;
    }
    *u = p_dot(dir, right) / denom;
    *v = p_dot(dir, up) / denom;
}

/* direction and nearest texel index of an unclamped face coordinate.
 * the coordinate may fall outside its own face, in which case the
 * direction is re-projected onto the neighbouring face. */
static void
reproject_tap(int face, float u, float v, int size, int *out_face, int *out_idx)
{
    float forward[3], up[3], right[3];
    float dir[3];

    probe_face_axes(face, forward, up, right);
    dir[0] = forward[0] + u * right[0] + v * up[0];
    dir[1] = forward[1] + u * right[1] + v * up[1];
    dir[2] = forward[2] + u * right[2] + v * up[2];
    p_normalize(dir);

    *out_face = dominant_face(dir);
    face_coords(dir, *out_face, &u, &v);

    /* nearest texel on the re-projected face; texel centres sit at
     * u = 2(x + .5)/size - 1, so x = floor((u + 1) * size / 2) */
    {
        int ix = (int)floorf((u + 1.0f) * 0.5f * (float)size);
        int iy = (int)floorf((1.0f - v) * 0.5f * (float)size);
        ix = clampi(ix, 0, size - 1);
        iy = clampi(iy, 0, size - 1);
        *out_idx = iy * size + ix;
    }
}

/* bilinear lookup of the directional distance moments.  taps that sit
 * outside the sampled face are re-projected through their own
 * direction instead of being clamped to the face edge. */

static void
sample_moment(struct probe_set *set, int probe, const float direction[3],
              float *distance, float *distance_squared)
{
    struct probe_generation *gen = &set->current;
    int size = set->config.face_size;
    int face = dominant_face(direction);
    float u, v, px, py;
    int ix, iy;
    float fx, fy;
    float mu = 0, m2 = 0;

    face_coords(direction, face, &u, &v);
    px = (u + 1.0f) * 0.5f * (float)size - 0.5f;
    py = (1.0f - v) * 0.5f * (float)size - 0.5f;

    ix = (int)floorf(px);
    iy = (int)floorf(py);
    fx = px - (float)ix;
    fy = py - (float)iy;

    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int tx = ix + i;
            int ty = iy + j;
            float wx = i ? fx : 1.0f - fx;
            float wy = j ? fy : 1.0f - fy;
            float w = wx * wy;
            struct probe_texel *image;
            int tap_face = face, idx;

            if (w <= 0)
                continue;

            if (tx >= 0 && tx < size && ty >= 0 && ty < size) {
                idx = ty * size + tx;
            } else {
                float tu = 2.0f * ((float)tx + 0.5f) / (float)size - 1.0f;
                float tv = 1.0f - 2.0f * ((float)ty + 0.5f) / (float)size;
                reproject_tap(face, tu, tv, size, &tap_face, &idx);
            }

            image = generation_face(gen, set, probe, tap_face);
            mu += w * image[idx].distance;
            m2 += w * image[idx].distance_squared;
        }
    }

    *distance = mu;
    *distance_squared = m2;
}

/********************************************************************
 *                                                                  *
 *                          public capture api                      *
 *                                                                  *
 ********************************************************************/

static void
generation_free(struct probe_generation *gen)
{
    free(gen->faces);
    free(gen->irradiance);
    gen->faces = NULL;
    gen->irradiance = NULL;
}

static int
generation_alloc(struct probe_generation *gen, int probes, int texels)
{
    size_t face_count = (size_t)probes * 6 * (size_t)texels;
    size_t coeff_count = (size_t)probes * 27;

    gen->faces = malloc(face_count * sizeof(struct probe_texel));
    gen->irradiance = malloc(coeff_count * sizeof(float));
    return gen->faces && gen->irradiance;
}

/********************************************************************
 *                                                                  *
 *                            lifecycle                             *
 *                                                                  *
 ********************************************************************/

static int
config_valid(const struct probe_config *c)
{
    if (!c)
        return 0;
    if (!p_finite3(c->origin))
        return 0;
    if (!(isfinite(c->spacing) && c->spacing > 0))
        return 0;
    if (c->nx < 2 || c->nx > 16 || c->ny < 2 || c->ny > 16 ||
        c->nz < 2 || c->nz > 16)
        return 0;
    if (c->face_size < 8 || c->face_size > 64)
        return 0;
    if (!(isfinite(c->near_plane) && c->near_plane > 0))
        return 0;
    if (!(isfinite(c->far_plane) && c->far_plane > c->near_plane))
        return 0;
    return 1;
}

static int
scene_valid(const struct probe_scene *s)
{
    if (!s)
        return 0;
    if (s->vertex_count < 0 || s->triangle_count < 0)
        return 0;
    if (s->triangle_count > 0 && (!s->triangles || !s->positions))
        return 0;
    if (s->vertex_count > 0 && !s->positions)
        return 0;
    for (int i = 0; i < s->vertex_count; i++) {
        if (!p_finite3(s->positions + i * 3))
            return 0;
    }
    for (int i = 0; i < s->triangle_count; i++) {
        const struct probe_triangle *t = s->triangles + i;
        if (!p_finite3(t->radiance))
            return 0;
        if (t->radiance[0] < 0 || t->radiance[1] < 0 || t->radiance[2] < 0)
            return 0;
        for (int j = 0; j < 3; j++) {
            if (t->index[j] < 0 || t->index[j] >= s->vertex_count)
                return 0;
        }
    }
    return 1;
}

/* builds expanded per-triangle capture vertices and planes */
static int
build_capture_geometry(struct probe_set *set, const struct probe_scene *scene)
{
    int count = 0;

    set->capture_pts = malloc((size_t)scene->triangle_count * 3 * 10 *
                              sizeof(float));
    set->capture_indices = malloc((size_t)scene->triangle_count * 3 *
                                  sizeof(int));
    if (scene->triangle_count > 0 &&
        (!set->capture_pts || !set->capture_indices))
        return 0;

    for (int i = 0; i < scene->triangle_count; i++) {
        const struct probe_triangle *t = scene->triangles + i;
        const float *a = scene->positions + t->index[0] * 3;
        const float *b = scene->positions + t->index[1] * 3;
        const float *c = scene->positions + t->index[2] * 3;
        float ab[3], ac[3], n[3];
        float len, d;

        ab[0] = b[0] - a[0]; ab[1] = b[1] - a[1]; ab[2] = b[2] - a[2];
        ac[0] = c[0] - a[0]; ac[1] = c[1] - a[1]; ac[2] = c[2] - a[2];
        p_cross(n, ab, ac);
        len = p_len(n);
        if (!(len > 1e-12f) || !isfinite(len))
            continue;   /* degenerate triangles emit nothing */
        n[0] /= len; n[1] /= len; n[2] /= len;
        d = -p_dot(n, a);

        for (int v = 0; v < 3; v++) {
            const float *p = scene->positions + t->index[v] * 3;
            float *out = set->capture_pts + (size_t)count * 30 + v * 10;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = n[0];
            out[4] = n[1];
            out[5] = n[2];
            out[6] = d;
            out[7] = t->radiance[0];
            out[8] = t->radiance[1];
            out[9] = t->radiance[2];
            set->capture_indices[count * 3 + v] = count * 3 + v;
        }
        count++;
    }

    set->capture_vertex_count = count * 3;
    return 1;
}

struct probe_set *
probe_create(const struct probe_config *config, const struct probe_scene *scene)
{
    struct probe_set *set;
    int probes, texels, size;

    if (!config_valid(config) || !scene_valid(scene))
        return NULL;

    set = calloc(1, sizeof(*set));
    if (!set)
        return NULL;

    set->config = *config;
    size = config->face_size;
    texels = size * size;
    probes = config->nx * config->ny * config->nz;

    /* copy the scene */
    set->triangle_count = scene->triangle_count;
    if (scene->triangle_count > 0) {
        set->triangles = malloc((size_t)scene->triangle_count *
                                sizeof(struct probe_triangle));
        if (!set->triangles)
            goto fail;
        memcpy(set->triangles, scene->triangles,
               (size_t)scene->triangle_count * sizeof(struct probe_triangle));
    }

    if (!build_capture_geometry(set, scene))
        goto fail;

    /* shared per-texel solid angles */
    set->solid_angle = malloc((size_t)6 * texels * sizeof(float));
    if (!set->solid_angle)
        goto fail;
    for (int f = 0; f < 6; f++)
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                set->solid_angle[(size_t)f * texels + y * size + x] =
                    probe_texel_solid_angle(f, x, y, size);

    set->scratch_colors = malloc((size_t)texels * sizeof(uint32_t));
    set->scratch_depths = malloc((size_t)texels * sizeof(float));
    if (!set->scratch_colors || !set->scratch_depths)
        goto fail;

    if (!generation_alloc(&set->current, probes, texels))
        goto fail;
    if (!generation_alloc(&set->pending, probes, texels))
        goto fail;

    return set;

fail:
    probe_destroy(set);
    return NULL;
}

void
probe_destroy(struct probe_set *set)
{
    if (!set)
        return;
    free(set->triangles);
    free(set->capture_pts);
    free(set->capture_indices);
    free(set->solid_angle);
    free(set->scratch_colors);
    free(set->scratch_depths);
    generation_free(&set->current);
    generation_free(&set->pending);
    free(set->volume);
    free(set);
}

/********************************************************************
 *                                                                  *
 *                          capture scheduling                      *
 *                                                                  *
 ********************************************************************/

int
probe_begin_update(struct probe_set *set, const float environment[3])
{
    float env[3] = {0, 0, 0};

    if (!set)
        return 0;
    if (environment) {
        if (!p_finite3(environment) || environment[0] < 0 ||
            environment[1] < 0 || environment[2] < 0)
            return 0;
        env[0] = environment[0];
        env[1] = environment[1];
        env[2] = environment[2];
    }

    set->environment[0] = env[0];
    set->environment[1] = env[1];
    set->environment[2] = env[2];

    /* a new update replaces any unfinished work; current is untouched */
    set->pending_active = 1;
    set->pending_probe = 0;
    set->pending_face = 0;

    return 1;
}

static void
publish_pending(struct probe_set *set)
{
    struct probe_generation tmp = set->current;
    set->current = set->pending;
    set->pending = tmp;
    set->has_current = 1;
    set->pending_active = 0;
}

int
probe_step(struct probe_set *set, int face_budget)
{
    int probes;

    if (!set || !set->pending_active || face_budget <= 0)
        return 0;

    probes = probe_count(set);
    while (face_budget > 0 && set->pending_probe < probes) {
        int probe = set->pending_probe;
        int face = set->pending_face;
        float *radiance = set->pending.irradiance + (size_t)probe * 27;

        /* the coefficient buffer doubles as a radiance accumulator
         * until the probe finishes its sixth face */
        if (face == 0)
            memset(radiance, 0, 27 * sizeof(float));

        capture_face(set, probe, face, &set->pending);

        {
            struct probe_texel *image =
                generation_face(&set->pending, set, probe, face);
            project_face(set, image, face, radiance);
        }

        face_budget--;
        set->pending_face++;
        if (set->pending_face == 6) {
            float irr[27];
            for (int c = 0; c < 3; c++)
                sh_lambert(radiance + c * 9, irr + c * 9);
            memcpy(radiance, irr, sizeof(irr));
            set->pending_face = 0;
            set->pending_probe++;
        }
    }

    if (set->pending_probe >= probes) {
        publish_pending(set);
        return 1;
    }
    return 0;
}

void
probe_abort(struct probe_set *set)
{
    if (!set)
        return;
    set->pending_active = 0;
    set->pending_probe = 0;
    set->pending_face = 0;
}

/********************************************************************
 *                                                                  *
 *                              queries                             *
 *                                                                  *
 ********************************************************************/

static void
trilinear_cell(const struct probe_set *set, const float p[3],
               int idx[8], float w[8])
{
    int n[3] = {set->config.nx, set->config.ny, set->config.nz};
    int base[3], top[3];
    float frac[3];

    for (int a = 0; a < 3; a++) {
        float f = (p[a] - set->config.origin[a]) / set->config.spacing;
        base[a] = (int)floorf(f);
        if (base[a] < 0) {
            base[a] = 0;
            frac[a] = 0;
        } else if (base[a] >= n[a] - 1) {
            base[a] = n[a] - 1;
            frac[a] = 0;
        } else {
            frac[a] = f - (float)base[a];
        }
        top[a] = clampi(base[a] + 1, 0, n[a] - 1);
    }

    for (int c = 0; c < 8; c++) {
        int x = (c & 1) ? top[0] : base[0];
        int y = (c & 2) ? top[1] : base[1];
        int z = (c & 4) ? top[2] : base[2];
        float wx = (c & 1) ? frac[0] : 1.0f - frac[0];
        float wy = (c & 2) ? frac[1] : 1.0f - frac[1];
        float wz = (c & 4) ? frac[2] : 1.0f - frac[2];
        idx[c] = (z * set->config.ny + y) * set->config.nx + x;
        w[c] = wx * wy * wz;
    }
}

int
probe_sample(const struct probe_set *set, const float p[3],
             const float n[3], float bias, float irradiance[3])
{
    int idx[8];
    float tri[8];
    float total = 0;
    float sum[3] = {0, 0, 0};
    float pb[3];
    float normal[3];
    float variance_floor;
    float nlen;

    if (!set || !set->has_current || !p || !n || !irradiance)
        return 0;
    if (!p_finite3(p) || !p_finite3(n))
        return 0;
    if (!(isfinite(bias) && bias >= 0))
        return 0;

    nlen = p_len(n);
    if (!(nlen > 0) || !isfinite(nlen))
        return 0;
    normal[0] = n[0] / nlen;
    normal[1] = n[1] / nlen;
    normal[2] = n[2] / nlen;

    pb[0] = p[0] + bias * normal[0];
    pb[1] = p[1] + bias * normal[1];
    pb[2] = p[2] + bias * normal[2];

    trilinear_cell(set, pb, idx, tri);
    variance_floor = 1e-4f * set->config.spacing * set->config.spacing;

    for (int c = 0; c < 8; c++) {
        float q[3], to_p[3], dir[3], r, mu, m2, variance;
        float visibility, normal_weight, weight;
        float basis[9];
        float e[3];

        if (tri[c] == 0)
            continue;

        probe_position(set, idx[c], q);
        to_p[0] = pb[0] - q[0];
        to_p[1] = pb[1] - q[1];
        to_p[2] = pb[2] - q[2];
        r = p_len(to_p);

        if (r == 0) {
            visibility = 1;
            normal_weight = 1;
        } else {
            float variance_floor_local = variance_floor;
            float nq[3];
            float ndot;

            dir[0] = to_p[0] / r;
            dir[1] = to_p[1] / r;
            dir[2] = to_p[2] / r;

            sample_moment((struct probe_set *)set, idx[c], dir, &mu, &m2);
            variance = m2 - mu * mu;
            if (variance < variance_floor_local)
                variance = variance_floor_local;

            if (r <= mu) {
                visibility = 1;
            } else {
                float diff = r - mu;
                visibility = variance / (variance + diff * diff);
            }

            nq[0] = q[0] - pb[0];
            nq[1] = q[1] - pb[1];
            nq[2] = q[2] - pb[2];
            p_normalize(nq);
            ndot = p_dot(normal, nq);
            if (ndot < 0.05f)
                ndot = 0.05f;
            normal_weight = ndot * ndot;
        }

        weight = tri[c] * visibility * visibility * visibility * normal_weight;
        if (weight <= 0)
            continue;

        sh_basis(normal, basis);
        for (int ch = 0; ch < 3; ch++) {
            const float *coeff =
                set->current.irradiance + (size_t)idx[c] * 27 + ch * 9;
            e[ch] = sh_reconstruct(coeff, basis);
        }

        sum[0] += weight * e[0];
        sum[1] += weight * e[1];
        sum[2] += weight * e[2];
        total += weight;
    }

    if (!(total > 0) || !isfinite(total))
        return 0;

    for (int c = 0; c < 3; c++) {
        float v = sum[c] / total;
        irradiance[c] = v < 0 ? 0 : v;
    }
    return 1;
}

int
probe_read_capture(const struct probe_set *set, int probe_index, int face,
                   const float **texels, int *face_size)
{
    if (!set || !set->has_current)
        return 0;
    if (probe_index < 0 || probe_index >= probe_count(set) ||
        face < 0 || face > 5)
        return 0;
    if (texels)
        *texels = (const float *)generation_face((struct probe_generation *)
                                                 &set->current, set,
                                                 probe_index, face);
    if (face_size)
        *face_size = set->config.face_size;
    return 1;
}

int
probe_read_coefficients(const struct probe_set *set, int probe_index,
                        float irradiance[27])
{
    if (!set || !set->has_current || !irradiance)
        return 0;
    if (probe_index < 0 || probe_index >= probe_count(set))
        return 0;
    memcpy(irradiance, set->current.irradiance + (size_t)probe_index * 27,
           27 * sizeof(float));
    return 1;
}

int
probe_read_distance_moment(const struct probe_set *set, int probe_index,
                           const float direction[3],
                           float *distance, float *distance_squared)
{
    float dir[3];
    if (!set || !set->has_current || !direction ||
        !distance || !distance_squared)
        return 0;
    if (probe_index < 0 || probe_index >= probe_count(set))
        return 0;
    if (!p_finite3(direction) || !(p_len(direction) > 0))
        return 0;
    dir[0] = direction[0];
    dir[1] = direction[1];
    dir[2] = direction[2];
    p_normalize(dir);
    sample_moment((struct probe_set *)set, probe_index, dir,
                  distance, distance_squared);
    return 1;
}

/********************************************************************
 *                                                                  *
 *                          phong binding                           *
 *                                                                  *
 ********************************************************************/

static int
probe_field_sample(void *field, const float *pos, const float *normal,
                   float irradiance[3])
{
    struct gl_probe_volume *volume = (struct gl_probe_volume *)field;
    return probe_sample(volume->set, pos, normal, volume->bias, irradiance);
}

struct gl_probe_volume *
probe_gl_volume(struct probe_set *set, float bias)
{
    if (!set)
        return NULL;
    if (!set->volume) {
        set->volume = calloc(1, sizeof(struct gl_probe_volume));
        if (!set->volume)
            return NULL;
    }
    set->volume->set = set;
    set->volume->bias = bias >= 0 ? bias : 0;
    return set->volume;
}

void
gl_bind_probe_volume(struct gl_probe_volume *volume)
{
    if (!volume)
        gl_bind_probe_field(NULL, NULL);
    else
        gl_bind_probe_field(volume, probe_field_sample);
}
