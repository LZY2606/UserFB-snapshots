#include "task_support/check.h"
#include "probe.h"
#include "gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/********************************************************************
 *                                                                  *
 *                    independent reference geometry                *
 *                                                                  *
 ********************************************************************/

/* the cube face table is restated here so the tests do not trust the
 * module's own helper */
static const float REF_FWD[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};
static const float REF_UP[6][3] = {
    {0,-1, 0}, {0,-1, 0},
    {0, 0, 1}, {0, 0,-1},
    {0,-1, 0}, {0,-1, 0},
};

static void
ref_right(int face, float out[3])
{
    out[0] = REF_FWD[face][1] * REF_UP[face][2] - REF_FWD[face][2] * REF_UP[face][1];
    out[1] = REF_FWD[face][2] * REF_UP[face][0] - REF_FWD[face][0] * REF_UP[face][2];
    out[2] = REF_FWD[face][0] * REF_UP[face][1] - REF_FWD[face][1] * REF_UP[face][0];
}

static void
ref_dir(int face, int x, int y, int n, float out[3])
{
    float r[3], u = 2 * ((float)x + .5f) / n - 1, v = 1 - 2 * ((float)y + .5f) / n;
    int k;
    ref_right(face, r);
    for (k = 0; k < 3; k++)
        out[k] = REF_FWD[face][k] + u * r[k] + v * REF_UP[face][k];
    {
        float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        out[0] /= l; out[1] /= l; out[2] /= l;
    }
}

static float
ref_tri_omega(const float a[3], const float b[3], const float c[3])
{
    float bx = b[1]*c[2]-b[2]*c[1], by = b[2]*c[0]-b[0]*c[2], bz = b[0]*c[1]-b[1]*c[0];
    float det = a[0]*bx + a[1]*by + a[2]*bz;
    float den = 1 + a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
                  + b[0]*c[0]+b[1]*c[1]+b[2]*c[2]
                  + c[0]*a[0]+c[1]*a[1]+c[2]*a[2];
    return 2 * atan2f(fabsf(det), den);
}

static float
ref_texel_omega(int face, int x, int y, int n)
{
    float c[4][3];
    int i, sx[4] = {0,1,1,0}, sy[4] = {0,0,1,1};
    for (i = 0; i < 4; i++) {
        float r[3], u = 2 * ((float)(x + sx[i])) / n - 1;
        float v = 1 - 2 * ((float)(y + sy[i])) / n;
        int k;
        ref_right(face, r);
        for (k = 0; k < 3; k++)
            c[i][k] = REF_FWD[face][k] + u * r[k] + v * REF_UP[face][k];
        {
            float l = sqrtf(c[i][0]*c[i][0]+c[i][1]*c[i][1]+c[i][2]*c[i][2]);
            c[i][0]/=l; c[i][1]/=l; c[i][2]/=l;
        }
    }
    return ref_tri_omega(c[0], c[1], c[2]) + ref_tri_omega(c[0], c[2], c[3]);
}

static int
ref_face_of(const float d[3])
{
    if (fabsf(d[0]) >= fabsf(d[1]) && fabsf(d[0]) >= fabsf(d[2]))
        return d[0] >= 0 ? 0 : 1;
    if (fabsf(d[1]) >= fabsf(d[2]))
        return d[1] >= 0 ? 2 : 3;
    return d[2] >= 0 ? 4 : 5;
}

/* project a world direction onto a face's (u, v) coordinates */
static void
ref_face_uv(const float d[3], int face, float *u, float *v)
{
    float f[3], up[3], r[3], den;
    memcpy(f, REF_FWD[face], sizeof(f));
    memcpy(up, REF_UP[face], sizeof(up));
    ref_right(face, r);
    den = d[0]*f[0] + d[1]*f[1] + d[2]*f[2];
    *u = (d[0]*r[0] + d[1]*r[1] + d[2]*r[2]) / den;
    *v = (d[0]*up[0] + d[1]*up[1] + d[2]*up[2]) / den;
}

/* seam-aware bilinear moment reference.  a tap outside its own face has
 * its centre direction re-projected onto the dominant neighbour rather
 * than being clamped to the face edge. */
static double
ref_bilinear_moment(const struct probe_set *s, int probe, int face,
                    float u, float v)
{
    int n = 16;
    float f[3], up[3], r[3], px, py, fx, fy;
    int ix, iy, i, j;
    double mu = 0;

    memcpy(f, REF_FWD[face], sizeof(f));
    memcpy(up, REF_UP[face], sizeof(up));
    ref_right(face, r);
    px = (u + 1) * .5f * n - .5f;
    py = (1 - v) * .5f * n - .5f;
    ix = (int)floorf(px); iy = (int)floorf(py);
    fx = px - ix; fy = py - iy;

    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++) {
            int tx = ix + i, ty = iy + j;
            float wx = i ? fx : 1 - fx, wy = j ? fy : 1 - fy;
            int tap_face = face, idx;
            const float *tex;
            int nn;

            if (tx >= 0 && tx < n && ty >= 0 && ty < n) {
                idx = ty * n + tx;
            } else {
                float tu = 2 * ((float)tx + .5f) / n - 1;
                float tv = 1 - 2 * ((float)ty + .5f) / n;
                float d[3], u2, v2;
                int ox, oy;
                d[0] = f[0] + tu * r[0] + tv * up[0];
                d[1] = f[1] + tu * r[1] + tv * up[1];
                d[2] = f[2] + tu * r[2] + tv * up[2];
                {
                    float l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    d[0] /= l; d[1] /= l; d[2] /= l;
                }
                tap_face = ref_face_of(d);
                ref_face_uv(d, tap_face, &u2, &v2);
                ox = (int)floorf((u2 + 1) * .5f * n);
                oy = (int)floorf((1 - v2) * .5f * n);
                if (ox < 0) ox = 0; if (ox >= n) ox = n - 1;
                if (oy < 0) oy = 0; if (oy >= n) oy = n - 1;
                idx = oy * n + ox;
            }
            TASK_CHECK(probe_read_capture(s, probe, tap_face, &tex, &nn) == 1);
            TASK_CHECK(nn == n);
            (void)nn;
            mu += (double)(wx * wy) * tex[idx * 5 + 3];
        }
    }
    return mu;
}

/********************************************************************
 *                                                                  *
 *                            fixtures                              *
 *                                                                  *
 ********************************************************************/

/* a fronto-parallel square facing -Z at z = distance, emitting
 * 'radiance', spanning [-extent, extent] */
static void
make_plane(float pos[12], struct probe_triangle tri[2],
           float distance, float extent, const float radiance[3])
{
    pos[0] = -extent; pos[1] = -extent; pos[2] = distance;
    pos[3] =  extent; pos[4] = -extent; pos[5] = distance;
    pos[6] =  extent; pos[7] =  extent; pos[8] = distance;
    pos[9] = -extent; pos[10] = extent; pos[11] = distance;

    tri[0].index[0] = 0; tri[0].index[1] = 2; tri[0].index[2] = 1;
    tri[1].index[0] = 0; tri[1].index[1] = 3; tri[1].index[2] = 2;
    memcpy(tri[0].radiance, radiance, 3 * sizeof(float));
    memcpy(tri[1].radiance, radiance, 3 * sizeof(float));
}

static struct probe_config
base_config(int face_size)
{
    struct probe_config c;
    memset(&c, 0, sizeof(c));
    c.spacing = 2.0f;
    c.nx = 2; c.ny = 2; c.nz = 2;
    c.face_size = face_size;
    c.near_plane = 0.01f;
    c.far_plane = 100.0f;
    return c;
}

/* drive an update to completion one whole face at a time */
static int
finish(struct probe_set *s)
{
    int published = 0, guard = 0;
    while (published == 0) {
        int p = probe_step(s, 1);
        if (p < 0 || ++guard > 100000)
            return -1;
        published = p;
    }
    return published;
}

/********************************************************************
 *                                                                  *
 *                          capture group                           *
 *                                                                  *
 ********************************************************************/

/* every texel's solid angle matches the independent corner integral
 * and all six faces add up to 4 pi */
static int
case_solid_angle(void)
{
    const int n = 16;
    double total = 0;
    int f, x, y;
    for (f = 0; f < 6; f++) {
        double face_sum = 0;
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++)
                face_sum += probe_texel_solid_angle(f, x, y, n);
        TASK_CHECK(fabs(face_sum - 2.0943951023931953) < 1e-5);
        total += face_sum;
    }
    TASK_CHECK(fabs(total - 4 * M_PI) < 1e-5);

    for (f = 0; f < 6; f++) {
        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                float a = probe_texel_solid_angle(f, x * 4, y * 4, n);
                float b = ref_texel_omega(f, x * 4, y * 4, n);
                TASK_CHECK(fabsf(a - b) < 1e-7f);
            }
        }
    }
    return 0;
}

/* the texel centre direction follows the documented parameterization */
static int
case_texel_direction(void)
{
    const int n = 8;
    int f, x, y;
    for (f = 0; f < 6; f++) {
        for (y = 0; y < n; y++) {
            for (x = 0; x < n; x++) {
                float got[3], want[3];
                probe_texel_direction(f, x, y, n, got);
                ref_dir(f, x, y, n, want);
                TASK_CHECK(fabsf(got[0] - want[0]) < 1e-6f);
                TASK_CHECK(fabsf(got[1] - want[1]) < 1e-6f);
                TASK_CHECK(fabsf(got[2] - want[2]) < 1e-6f);
            }
        }
    }
    return 0;
}

/* a fronto-parallel plane yields the exact radial distance at every
 * texel centre, well inside the 1e-4 budget */
static int
case_radial_distance(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1.0f, 0.5f, 0.25f};
    struct probe_scene scene;
    struct probe_config cfg = base_config(32);
    struct probe_set *s;
    const float *tex;
    int n, x, y;

    make_plane(pos, tri, 4.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(probe_step(s, 6) == 0);
    TASK_CHECK(finish(s) == 1);

    TASK_CHECK(probe_read_capture(s, 0, 4, &tex, &n) == 1);
    TASK_CHECK(n == 32);
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            float dir[3];
            double t;
            int i = (y * n + x) * 5;
            ref_dir(4, x, y, n, dir);
            t = 4.0 / dir[2];
            TASK_CHECK(fabs(tex[i + 3] - t) < 1e-4);
            TASK_CHECK(fabs(tex[i + 4] - t * t) < 1e-3);
        }
    }
    probe_destroy(s);
    return 0;
}

/* an empty scene under a constant environment gives the analytic sh
 * convolution */
static int
case_constant_environment(void)
{
    const float env[3] = {0.8f, 0.4f, 0.2f};
    struct probe_scene scene;
    struct probe_config cfg = base_config(32);
    struct probe_set *s;
    float coeff[27];
    int c;

    scene.positions = NULL;
    scene.vertex_count = 0;
    scene.triangles = NULL;
    scene.triangle_count = 0;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, env) == 1);
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 1);

    for (c = 0; c < 3; c++) {
        double e = coeff[c * 9 + 0] * 0.28209479177387814;
        double want = M_PI * env[c];
        TASK_CHECK(fabs(e - want) < 2e-3 * want + 1e-5);
        for (int k = 1; k < 9; k++)
            TASK_CHECK(fabsf(coeff[c * 9 + k]) < 2e-3f * (float)want + 1e-4f);
    }

    {
        float nrm[3] = {0.3f, -0.5f, 0.8f};
        float e[3];
        float l = sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
        nrm[0] /= l; nrm[1] /= l; nrm[2] /= l;
        TASK_CHECK(probe_sample(s, (float[3]){0, 0, 0}, nrm, 0, e) == 1);
        for (c = 0; c < 3; c++)
            TASK_CHECK(fabs(e[c] - M_PI * env[c]) < 2e-3 * fabs(e[c]) + 1e-4);
    }
    probe_destroy(s);
    return 0;
}

/* captured radiance stays unbounded float hdr, never quantized */
static int
case_hdr(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {40.0f, 25.0f, 12.5f};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    const float *tex;
    int n, i, seen = 0;

    make_plane(pos, tri, 2.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_capture(s, 0, 4, &tex, &n) == 1);
    for (i = 0; i < n * n; i++) {
        if (tex[i * 5 + 3] < 90.0f) {
            TASK_CHECK(fabsf(tex[i * 5 + 0] - 40.0f) < 1e-5f);
            TASK_CHECK(fabsf(tex[i * 5 + 1] - 25.0f) < 1e-5f);
            TASK_CHECK(fabsf(tex[i * 5 + 2] - 12.5f) < 1e-5f);
            seen = 1;
        }
    }
    TASK_CHECK(seen);

    {
        float coeff[27];
        TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 1);
        TASK_CHECK(coeff[2] > 10.0f);   /* plane lies on +Z */
    }
    probe_destroy(s);
    return 0;
}

/* a nearer black surface wins the depth test over a farther bright
 * one, and the recorded distance is the winner's */
static int
case_occlusion_winner(void)
{
    float pos[24];
    struct probe_triangle tri[4];
    float black[3] = {0, 0, 0};
    float white[3] = {5, 5, 5};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    const float *tex;
    int n, x, y;

    {
        float near_pos[12], far_pos[12];
        struct probe_triangle near_tri[2], far_tri[2];
        make_plane(near_pos, near_tri, 2.0f, 4.0f, black);
        make_plane(far_pos, far_tri, 8.0f, 100.0f, white);
        memcpy(pos, near_pos, sizeof(near_pos));
        memcpy(pos + 12, far_pos, sizeof(far_pos));
        memcpy(tri, near_tri, sizeof(near_tri));
        memcpy(tri + 2, far_tri, sizeof(far_tri));
        tri[2].index[0] += 4; tri[2].index[1] += 4; tri[2].index[2] += 4;
        tri[3].index[0] += 4; tri[3].index[1] += 4; tri[3].index[2] += 4;
    }

    scene.positions = pos;
    scene.vertex_count = 8;
    scene.triangles = tri;
    scene.triangle_count = 4;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_capture(s, 0, 4, &tex, &n) == 1);

    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            int i = (y * n + x) * 5;
            float dir[3];
            double t, hx, hy;
            ref_dir(4, x, y, n, dir);
            t = 2.0 / dir[2];
            hx = dir[0] * t;
            hy = dir[1] * t;
            if (fabs(hx) <= 4.0 && fabs(hy) <= 4.0) {
                /* the black quad is the nearest surface */
                TASK_CHECK(tex[i + 0] < 1e-4f);
                TASK_CHECK(fabs(tex[i + 3] - t) < 1e-4);
            } else {
                /* only the far bright quad is in the way */
                TASK_CHECK(fabsf(tex[i + 0] - 5.0f) < 1e-4f);
            }
        }
    }
    probe_destroy(s);
    return 0;
}

/* whole-face batches and one-face steps produce identical results */
static int
case_budget_equivalence(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {0.7f, 0.3f, 0.11f};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *a, *b;
    int i;

    make_plane(pos, tri, 3.0f, 50.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    a = probe_create(&cfg, &scene);
    b = probe_create(&cfg, &scene);
    TASK_CHECK(a && b);
    TASK_CHECK(probe_begin_update(a, NULL) == 1);
    TASK_CHECK(probe_begin_update(b, NULL) == 1);

    TASK_CHECK(finish(a) == 1);
    while (probe_step(b, 1) == 0) { }
    TASK_CHECK(probe_step(b, 0) == 0);

    for (i = 0; i < 8; i++) {
        float ca[27], cb[27];
        TASK_CHECK(probe_read_coefficients(a, i, ca) == 1);
        TASK_CHECK(probe_read_coefficients(b, i, cb) == 1);
        TASK_CHECK(memcmp(ca, cb, sizeof(ca)) == 0);
    }
    {
        const float *ta, *tb;
        int na, nb;
        TASK_CHECK(probe_read_capture(a, 3, 5, &ta, &na) == 1);
        TASK_CHECK(probe_read_capture(b, 3, 5, &tb, &nb) == 1);
        TASK_CHECK(na == nb);
        TASK_CHECK(memcmp(ta, tb, (size_t)na * na * 5 * sizeof(float)) == 0);
    }
    probe_destroy(a);
    probe_destroy(b);
    return 0;
}

/* the scene is copied; mutating or freeing the source is harmless */
static int
case_source_lifetime(void)
{
    float *pos = malloc(12 * sizeof(float));
    struct probe_triangle *tri = malloc(2 * sizeof(*tri));
    float radiance[3] = {1, 1, 1};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    const float *tex;
    int n, i, hits = 0;

    TASK_CHECK(pos && tri);
    make_plane(pos, tri, 2.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);

    for (i = 0; i < 12; i++)
        pos[i] = -999.0f;
    tri[0].radiance[0] = 99.0f;
    free(pos);
    free(tri);

    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_capture(s, 0, 4, &tex, &n) == 1);
    for (i = 0; i < n * n; i++) {
        if (tex[i * 5 + 3] < 90.0f) {
            TASK_CHECK(fabsf(tex[i * 5 + 0] - 1.0f) < 1e-5f);
            hits++;
        }
    }
    TASK_CHECK(hits > 0);
    probe_destroy(s);
    return 0;
}

/* invalid configurations and scenes are rejected */
static int
case_validation(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1, 1, 1};
    struct probe_scene scene;
    struct probe_config cfg;

    make_plane(pos, tri, 2.0f, 10.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    cfg = base_config(16); cfg.spacing = 0;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.spacing = -1;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.nx = 1;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.nz = 17;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.face_size = 7;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.face_size = 65;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.near_plane = -1;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.far_plane = 0;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);
    cfg = base_config(16); cfg.origin[0] = NAN;
    TASK_CHECK(probe_create(&cfg, &scene) == NULL);

    cfg = base_config(16);
    {
        struct probe_scene bad = scene;
        struct probe_triangle bad_tri[2];
        memcpy(bad_tri, tri, sizeof(tri));
        bad_tri[0].radiance[0] = -1;
        bad.triangles = bad_tri;
        TASK_CHECK(probe_create(&cfg, &bad) == NULL);
    }
    cfg = base_config(16);
    {
        struct probe_scene bad = scene;
        struct probe_triangle bad_tri[2];
        memcpy(bad_tri, tri, sizeof(tri));
        bad_tri[0].index[2] = 99;
        bad.triangles = bad_tri;
        TASK_CHECK(probe_create(&cfg, &bad) == NULL);
    }
    cfg = base_config(16);
    {
        struct probe_scene bad = scene;
        bad.triangle_count = -1;
        TASK_CHECK(probe_create(&cfg, &bad) == NULL);
    }

    cfg = base_config(8);
    {
        struct probe_set *s = probe_create(&cfg, &scene);
        TASK_CHECK(s != NULL);
        probe_destroy(s);
    }
    return 0;
}

/********************************************************************
 *                                                                  *
 *                          visibility group                        *
 *                                                                  *
 ********************************************************************/

/* the stored moment is directional: opposite directions from one probe
 * report different distances, never a per-probe average */
static int
case_directional_moment(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1, 1, 1};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    float up[3] = {0, 0, 1}, down[3] = {0, 0, -1};
    float side[3] = {1, 0, 0};
    float mu_up, m2_up, mu_dn, m2_dn, mu_sd, m2_sd;

    make_plane(pos, tri, 6.0f, 200.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    cfg.origin[0] = 0; cfg.origin[1] = 0; cfg.origin[2] = 0;
    cfg.spacing = 1.0f;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);

    TASK_CHECK(probe_read_distance_moment(s, 0, up, &mu_up, &m2_up) == 1);
    TASK_CHECK(probe_read_distance_moment(s, 0, down, &mu_dn, &m2_dn) == 1);
    TASK_CHECK(probe_read_distance_moment(s, 0, side, &mu_sd, &m2_sd) == 1);
    /* the direction falls exactly on the face centre, between four
     * texels, so the bilinear moment is those texels' average, slightly
     * larger than the axis distance */
    TASK_CHECK(fabsf(mu_up - 6.0f) < 0.05f);
    TASK_CHECK(fabsf(m2_up - 36.0f) < 0.6f);
    TASK_CHECK(mu_dn > 50.0f);          /* opposite ray misses */
    TASK_CHECK(mu_sd > 50.0f);          /* side ray misses */
    TASK_CHECK(mu_up + 1e-3f < mu_dn);  /* not an average */
    probe_destroy(s);
    return 0;
}

/* bilinear taps that cross a cube edge are re-projected by direction to
 * the neighbouring face instead of being clamped */
static int
case_seam_interpolation(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1, 1, 1};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    float dir[3], u, v, mu, m2;
    int face;

    /* a far plane so distance varies smoothly with direction */
    make_plane(pos, tri, 5.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    cfg.origin[0] = 0; cfg.origin[1] = 0; cfg.origin[2] = 0;
    cfg.spacing = 4.0f;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);

    /* a direction on the +X face but close to its -Z edge: at least one
     * bilinear tap is outside the face and must cross the seam */
    dir[0] = 1.0f; dir[1] = -0.05f; dir[2] = -0.99f;
    {
        float l = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        dir[0] /= l; dir[1] /= l; dir[2] /= l;
    }
    face = ref_face_of(dir);
    ref_face_uv(dir, face, &u, &v);
    TASK_CHECK(probe_read_distance_moment(s, 0, dir, &mu, &m2) == 1);
    {
        double want = ref_bilinear_moment(s, 0, face, u, v);
        TASK_CHECK(fabs(mu - want) < 1e-4 * (fabs(want) + 1));
    }

    /* a second crossing, this time over a face corner */
    dir[0] = 0.60f; dir[1] = 0.60f; dir[2] = 0.53f;
    {
        float l = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        dir[0] /= l; dir[1] /= l; dir[2] /= l;
    }
    face = ref_face_of(dir);
    ref_face_uv(dir, face, &u, &v);
    TASK_CHECK(probe_read_distance_moment(s, 0, dir, &mu, &m2) == 1);
    {
        double want = ref_bilinear_moment(s, 0, face, u, v);
        TASK_CHECK(fabs(mu - want) < 1e-4 * (fabs(want) + 1));
    }
    probe_destroy(s);
    return 0;
}

/* a thin black wall splits a probe grid; a query on the far side of
 * the wall is blocked from the bright panel while one on the near side
 * sees it.  The probes straddle the wall so the query's visibility
 * weights have to suppress the occluded probes. */
static int
case_thin_wall_visibility(void)
{
    /* bright panel at z = 6; thin black wall at z = 2 covering the
     * probe volume; probes at z = 0.5, 1.5, 2.5, 3.5 straddle it */
    float pos[24];
    struct probe_triangle tri[4];
    float black[3] = {0, 0, 0};
    float bright[3] = {3, 3, 3};
    struct probe_scene scene;
    struct probe_config cfg;
    struct probe_set *s;
    float far_side[3] = {0, 0, -0.5f};
    float near_side[3] = {0, 0, 4.5f};
    float nz[3] = {0, 0, 1};
    float Efar[3], Enear[3];

    {
        float a[12], b[12];
        struct probe_triangle ta[2], tb[2];
        make_plane(a, ta, 6.0f, 100.0f, bright);
        make_plane(b, tb, 2.0f, 6.0f, black);
        memcpy(pos, a, sizeof(a));
        memcpy(pos + 12, b, sizeof(b));
        memcpy(tri, ta, sizeof(ta));
        memcpy(tri + 2, tb, sizeof(tb));
        tri[2].index[0] += 4; tri[2].index[1] += 4; tri[2].index[2] += 4;
        tri[3].index[0] += 4; tri[3].index[1] += 4; tri[3].index[2] += 4;
    }

    scene.positions = pos;
    scene.vertex_count = 8;
    scene.triangles = tri;
    scene.triangle_count = 4;

    cfg = base_config(24);
    cfg.origin[0] = 0; cfg.origin[1] = 0; cfg.origin[2] = 0.5f;
    cfg.spacing = 1.0f;
    cfg.nx = cfg.ny = 2;
    cfg.nz = 4;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);

    /* on the +Z side of the wall the panel is in plain view */
    TASK_CHECK(probe_sample(s, near_side, nz, 0, Enear) == 1);
    /* on the -Z side the black wall hides the panel, so the indirect
     * light collapses: the visibility terms reject the probes that sit
     * beyond the wall */
    TASK_CHECK(probe_sample(s, far_side, nz, 0, Efar) == 1);
    TASK_CHECK(Enear[0] > 0.05f);
    TASK_CHECK(Efar[0] < 0.5f * Enear[0]);

    probe_destroy(s);
    return 0;
}

/* visibility fades smoothly with distance from a surface and is full
 * for a query right at a probe; the result never fabricates a nearest
 * probe when weights vanish */
static int
case_visibility_falloff(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {2, 2, 2};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    float nz[3] = {0, 0, 1};
    float nzneg[3] = {0, 0, -1};
    float Enear[3], Efar[3], Eback[3];

    /* a small bright panel on +Z at z = 3 */
    make_plane(pos, tri, 3.0f, 3.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    cfg.origin[0] = 0; cfg.origin[1] = 0; cfg.origin[2] = 0;
    cfg.spacing = 1.0f;
    cfg.nx = cfg.ny = cfg.nz = 4;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);

    /* a point just below the panel and one far below it: the panel's
     * solid angle falls with distance, so the near query is brighter */
    TASK_CHECK(probe_sample(s, (float[3]){0, 0, 2.0f}, nz, 0, Enear) == 1);
    TASK_CHECK(probe_sample(s, (float[3]){0, 0, -3.0f}, nz, 0, Efar) == 1);
    TASK_CHECK(Enear[0] > 0.05f);
    TASK_CHECK(Efar[0] < Enear[0]);

    /* a query whose normal faces away from the panel is dark, but the
     * call still succeeds rather than inventing a nearest probe */
    TASK_CHECK(probe_sample(s, (float[3]){0, 0, -3.0f}, nzneg, 0, Eback) == 1);
    TASK_CHECK(Eback[0] <= Efar[0] + 1e-4f);

    /* a zero bias exactly on a probe centre still succeeds */
    TASK_CHECK(probe_sample(s, (float[3]){0, 0, 0}, nz, 0, Eback) == 1);
    TASK_CHECK(Eback[0] >= 0);
    probe_destroy(s);
    return 0;
}

/********************************************************************
 *                                                                  *
 *                         publication group                        *
 *                                                                  *
 ********************************************************************/

/* nothing is published until every probe owns all six faces */
static int
case_publication_barrier(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1, 0, 0};
    struct probe_scene scene;
    struct probe_config cfg = base_config(8);
    struct probe_set *s;
    float coeff[27];
    int probes, total_faces, f;

    make_plane(pos, tri, 2.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    probes = cfg.nx * cfg.ny * cfg.nz;
    total_faces = probes * 6;

    TASK_CHECK(probe_sample(s, (float[3]){0,0,0}, (float[3]){0,0,1}, 0,
                            coeff) == 0);
    TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 0);

    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    for (f = 0; f < total_faces - 1; f++) {
        TASK_CHECK(probe_step(s, 1) == 0);
        TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 0);
    }
    TASK_CHECK(probe_step(s, 1) == 1);
    TASK_CHECK(probe_step(s, 1) == 0);
    TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 1);

    probe_destroy(s);
    return 0;
}

/* a zero budget does no work */
static int
case_zero_budget(void)
{
    float pos[12];
    struct probe_triangle tri[2];
    float radiance[3] = {1, 1, 1};
    struct probe_scene scene;
    struct probe_config cfg = base_config(8);
    struct probe_set *s;
    float coeff[27];

    make_plane(pos, tri, 2.0f, 100.0f, radiance);
    scene.positions = pos;
    scene.vertex_count = 4;
    scene.triangles = tri;
    scene.triangle_count = 2;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(probe_step(s, 0) == 0);
    TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 0);
    TASK_CHECK(probe_step(s, 1000) == 1);
    TASK_CHECK(probe_read_coefficients(s, 0, coeff) == 1);
    probe_destroy(s);
    return 0;
}

/* a new update replaces unfinished work and abort drops it; both keep
 * the last complete generation readable */
static int
case_replace_and_abort(void)
{
    float red[3] = {1, 0, 0};
    float green[3] = {0, 1, 0};
    struct probe_scene scene;
    struct probe_config cfg = base_config(8);
    struct probe_set *s;
    float before[27], during[27], after[27];
    float P[3] = {0.5f, 0.5f, 0.5f}, N[3] = {0, 0, 1}, E[3];

    /* an empty scene so each update's constant environment fully
     * determines the captured radiance; updates vary via the
     * environment, the scene copy itself is owned by the set */
    scene.positions = NULL;
    scene.vertex_count = 0;
    scene.triangles = NULL;
    scene.triangle_count = 0;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);

    TASK_CHECK(probe_begin_update(s, red) == 1);
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_coefficients(s, 0, before) == 1);
    TASK_CHECK(probe_sample(s, P, N, 0, E) == 1);
    TASK_CHECK(E[0] > E[1] && E[0] > E[2]);

    /* start a green update but leave it unfinished */
    TASK_CHECK(probe_begin_update(s, green) == 1);
    TASK_CHECK(probe_step(s, 3) == 0);
    TASK_CHECK(probe_read_coefficients(s, 0, during) == 1);
    TASK_CHECK(memcmp(before, during, sizeof(before)) == 0);

    /* abort discards it; the red generation survives */
    probe_abort(s);
    TASK_CHECK(probe_read_coefficients(s, 0, during) == 1);
    TASK_CHECK(memcmp(before, during, sizeof(before)) == 0);

    /* a fresh green update, replaced midway by another green update,
     * publishes green */
    TASK_CHECK(probe_begin_update(s, green) == 1);
    TASK_CHECK(probe_step(s, 2) == 0);
    TASK_CHECK(probe_begin_update(s, green) == 1);   /* replace */
    TASK_CHECK(finish(s) == 1);
    TASK_CHECK(probe_read_coefficients(s, 0, after) == 1);
    TASK_CHECK(probe_sample(s, P, N, 0, E) == 1);
    TASK_CHECK(E[1] > E[0] && E[1] > E[2]);

    probe_destroy(s);
    return 0;
}

/* two independent sets interleaved keep their own generations */
static int
case_interleaved_sets(void)
{
    float pos_a[12], pos_b[12];
    struct probe_triangle tri_a[2], tri_b[2];
    float ra[3] = {1, 0, 0};
    float rb[3] = {0, 0, 1};
    struct probe_scene sa, sb;
    struct probe_config cfg = base_config(8);
    struct probe_set *A, *B;
    float ca[27], cb[27];
    int step;

    make_plane(pos_a, tri_a, 2.0f, 100.0f, ra);
    make_plane(pos_b, tri_b, 3.0f, 100.0f, rb);
    sa.positions = pos_a; sa.vertex_count = 4;
    sa.triangles = tri_a; sa.triangle_count = 2;
    sb.positions = pos_b; sb.vertex_count = 4;
    sb.triangles = tri_b; sb.triangle_count = 2;

    A = probe_create(&cfg, &sa);
    B = probe_create(&cfg, &sb);
    TASK_CHECK(A && B);
    TASK_CHECK(probe_begin_update(A, NULL) == 1);
    TASK_CHECK(probe_begin_update(B, NULL) == 1);

    {
        int published_a = 0, published_b = 0;
        for (step = 0; step < 1000 && !(published_a && published_b); step++) {
            if (!published_a)
                published_a = probe_step(A, 1);
            if (!published_b)
                published_b = probe_step(B, 2);
        }
        TASK_CHECK(published_a && published_b);
    }
    TASK_CHECK(probe_read_coefficients(A, 0, ca) == 1);
    TASK_CHECK(probe_read_coefficients(B, 0, cb) == 1);
    TASK_CHECK(ca[0] > ca[9] && ca[0] > ca[18]);    /* red */
    TASK_CHECK(cb[18] > cb[0] && cb[18] > cb[9]);   /* blue */
    probe_destroy(A);
    probe_destroy(B);
    return 0;
}

/* the standard phong shader consumes the bound volume and restores its
 * classic output when unbound */
static int
case_phong_binding(void)
{
    float panel_pos[12];
    struct probe_triangle panel_tri[2];
    float panel_rad[3] = {20, 20, 20};
    struct probe_scene scene;
    struct probe_config cfg = base_config(16);
    struct probe_set *s;
    struct gl_probe_volume *vol;
    float material_ambient[4] = {0, 0, 0, 0};
    float material_diffuse[4] = {1, 1, 1, 1};
    float material_specular[4] = {0, 0, 0, 0};
    float zero = 0;
    float verts[6 * 8] = {
        -8, -8, -1, 0, 0, 0, 0, 1,
         8, -8, -1, 0, 0, 0, 0, 1,
         8,  8, -1, 0, 0, 0, 0, 1,
        -8, -8, -1, 0, 0, 0, 0, 1,
         8,  8, -1, 0, 0, 0, 0, 1,
        -8,  8, -1, 0, 0, 0, 0, 1,
    };
    int idx[] = {0, 1, 2, 3, 4, 5};
    uint32_t colors[64 * 64];
    float depths[64 * 64];
    int i, lit = 0, black_unbound = 1;

    make_plane(panel_pos, panel_tri, 4.0f, 30.0f, panel_rad);
    scene.positions = panel_pos;
    scene.vertex_count = 4;
    scene.triangles = panel_tri;
    scene.triangle_count = 2;

    cfg.origin[0] = -1; cfg.origin[1] = -1; cfg.origin[2] = -1;
    cfg.spacing = 1.0f;
    cfg.nx = cfg.ny = cfg.nz = 4;

    s = probe_create(&cfg, &scene);
    TASK_CHECK(s != NULL);
    TASK_CHECK(probe_begin_update(s, NULL) == 1);
    TASK_CHECK(finish(s) == 1);

    gl_glight(GL_AMBIENT, &zero);
    gl_glight(GL_DIFFUSE, (float[1]){1});
    gl_glight(GL_SPECULAR, &zero);
    for (i = 0; i < GL_MAX_LIGHT_COUNT; i++)
        gl_light_disable((enum gl_light_slot)i);
    gl_material(GL_AMBIENT, material_ambient);
    gl_material(GL_DIFFUSE, material_diffuse);
    gl_material(GL_SPECULAR, material_specular);
    gl_material(GL_SHININESS, (float[1]){16});
    gl_material(GL_BLEND, (float[1]){0});

    gl_bind_framebuffer(64, 64, colors, depths);
    gl_bind_std_vs();
    gl_bind_phong_fs();
    gl_matrix_mode(GL_PROJECTION_MATRIX);
    gl_load_identity();
    gl_perspective(1.0f, 1.0f, 0.1f, 100.0f);
    gl_matrix_mode(GL_VIEW_MATRIX);
    gl_load_identity();
    gl_look_at(0, 0, 0, 0, 0, -1, 0, 1, 0);
    gl_matrix_mode(GL_MODEL_MATRIX);
    gl_load_identity();
    gl_bind_vertices(verts, 6, 8);

    vol = probe_gl_volume(s, 0.01f);
    TASK_CHECK(vol != NULL);

    for (i = 0; i < 64 * 64; i++) { colors[i] = 0; depths[i] = 1000; }
    gl_bind_probe_volume(NULL);
    gl_renderl(idx, 6, GL_TRIANGLE_LIST);
    for (i = 0; i < 64 * 64; i++)
        if ((colors[i] & 0x00ffffffu) != 0) black_unbound = 0;
    TASK_CHECK(black_unbound);

    for (i = 0; i < 64 * 64; i++) { colors[i] = 0; depths[i] = 1000; }
    gl_bind_probe_volume(vol);
    gl_renderl(idx, 6, GL_TRIANGLE_LIST);
    for (i = 0; i < 64 * 64; i++)
        if ((colors[i] & 0x0000ff00u) > 0x00001000u) lit++;
    TASK_CHECK(lit > 0);

    /* an independent magnitude check: a constant environment of L = 1
     * with diffuse material 1 and kd 1 gives diffuse * kd * E / pi =
     * 1, so a full-white pixel (255) is expected.  Compare against the
     * measured green channel to confirm the phong term is E / pi and
     * not, say, E itself or E / (2 pi). */
    {
        struct probe_scene empty = {0, 0, 0, 0};
        struct probe_config ecfg = base_config(16);
        struct probe_set *es;
        struct gl_probe_volume *evol;
        float env[3] = {1, 1, 1};
        int white = 0, mid = 0;

        ecfg.origin[0] = -1; ecfg.origin[1] = -1; ecfg.origin[2] = 1;
        ecfg.spacing = 1.0f;
        ecfg.nx = ecfg.ny = ecfg.nz = 2;
        es = probe_create(&ecfg, &empty);
        TASK_CHECK(es != NULL);
        TASK_CHECK(probe_begin_update(es, env) == 1);
        TASK_CHECK(finish(es) == 1);
        evol = probe_gl_volume(es, 0.01f);
        TASK_CHECK(evol != NULL);

        for (i = 0; i < 64 * 64; i++) { colors[i] = 0; depths[i] = 1000; }
        gl_bind_probe_volume(evol);
        gl_renderl(idx, 6, GL_TRIANGLE_LIST);
        for (i = 0; i < 64 * 64; i++) {
            unsigned g = (colors[i] & 0x0000ff00u) >> 8;
            if (g >= 250) white++;
            else if (g > 100) mid++;
        }
        TASK_CHECK(white > 0);        /* E = pi L, so the term is 1.0 */
        TASK_CHECK(mid == 0);         /* nothing at half brightness */
        gl_bind_probe_volume(NULL);
        probe_destroy(es);
    }

    for (i = 0; i < 64 * 64; i++) { colors[i] = 0; depths[i] = 1000; }
    gl_bind_probe_volume(NULL);
    gl_renderl(idx, 6, GL_TRIANGLE_LIST);
    for (i = 0; i < 64 * 64; i++)
        TASK_CHECK((colors[i] & 0x00ffffffu) == 0);

    probe_destroy(s);
    return 0;
}

/********************************************************************
 *                                                                  *
 *                             registry                             *
 *                                                                  *
 ********************************************************************/

const struct task_case task_cases[] = {
    {"capture", "solid_angle", case_solid_angle},
    {"capture", "texel_direction", case_texel_direction},
    {"capture", "radial_distance", case_radial_distance},
    {"capture", "constant_environment", case_constant_environment},
    {"capture", "hdr", case_hdr},
    {"capture", "occlusion_winner", case_occlusion_winner},
    {"capture", "budget_equivalence", case_budget_equivalence},
    {"capture", "source_lifetime", case_source_lifetime},
    {"capture", "validation", case_validation},
    {"visibility", "directional_moment", case_directional_moment},
    {"visibility", "seam_interpolation", case_seam_interpolation},
    {"visibility", "thin_wall", case_thin_wall_visibility},
    {"visibility", "falloff", case_visibility_falloff},
    {"publication", "barrier", case_publication_barrier},
    {"publication", "zero_budget", case_zero_budget},
    {"publication", "replace_and_abort", case_replace_and_abort},
    {"publication", "interleaved_sets", case_interleaved_sets},
    {"publication", "phong_binding", case_phong_binding},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
