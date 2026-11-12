// Regression contracts for par_shapes_create_sweep.  Every contract uses its
// own frame transport and its own area-weighted normal accumulation so that
// the library implementation is checked against an independent derivation.

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "par_shapes.h"

static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
            #cond); \
        return -g_checks; \
    } \
} while (0)

// -- Independent reference math ----------------------------------------------

static void rv_sub(float out[3], float const a[3], float const b[3])
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static float rv_dot(float const a[3], float const b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void rv_cross(float out[3], float const a[3], float const b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void rv_norm(float v[3])
{
    float l = sqrtf(rv_dot(v, v));
    if (l > 0.0f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static void rv_rodrigues(float out[3], float const v[3],
    float const axis[3], float ang)
{
    float cr[3];
    rv_cross(cr, axis, v);
    float k = rv_dot(axis, v) * (1.0f - cosf(ang));
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] * cosf(ang) + cr[i] * sinf(ang) + axis[i] * k;
    }
}

// Shortest rotation taking unit vector "from" to unit vector "to".
static int rv_transport(float out[3], float const v[3],
    float const from[3], float const to[3])
{
    float axis[3];
    rv_cross(axis, from, to);
    float axis2 = rv_dot(axis, axis);
    float d = rv_dot(from, to);
    if (axis2 > 1e-20f) {
        rv_norm(axis);
        rv_rodrigues(out, v, axis, atan2f(sqrtf(axis2), d));
        return 1;
    }
    if (d > 0.0f) {
        memcpy(out, v, 3 * sizeof(float));
        return 1;
    }
    return 0;
}

static float rv_signed_angle(float const a[3], float const b[3],
    float const axis[3])
{
    float cr[3];
    rv_cross(cr, a, b);
    return atan2f(rv_dot(cr, axis), rv_dot(a, b));
}

// Independent re-derivation of every tangent and major axis.  Returns 0 on
// success and -1 when the same discrete rules leave a frame undefined.
static int rv_build_frames(float const* centers, int n, int closed,
    float const initial[3], float* tang, float* maj, float* cum,
    float* total_out, float* twist_out)
{
    int nseg = closed ? n : n - 1;
    float total = 0.0f;
    cum[0] = 0.0f;
    for (int s = 0; s < nseg; s++) {
        float d[3];
        rv_sub(d, centers + 3 * ((s + 1) % n), centers + 3 * s);
        float len = sqrtf(rv_dot(d, d));
        if (!(len > 0.0f)) {
            return -1;
        }
        total += len;
        cum[s + 1] = total;
    }
    for (int i = 0; i < n; i++) {
        int ids[2], nids;
        if (!closed && i == 0) {
            ids[0] = 0;
            nids = 1;
        } else if (!closed && i == n - 1) {
            ids[0] = nseg - 1;
            nids = 1;
        } else {
            ids[0] = closed ? (i + n - 1) % n : i - 1;
            ids[1] = i;
            nids = 2;
        }
        float sum[3] = {0, 0, 0};
        for (int q = 0; q < nids; q++) {
            int s = ids[q];
            float d[3];
            rv_sub(d, centers + 3 * ((s + 1) % n), centers + 3 * s);
            rv_norm(d);
            sum[0] += d[0];
            sum[1] += d[1];
            sum[2] += d[2];
        }
        if (rv_dot(sum, sum) <= 1e-12f) {
            return -1;
        }
        rv_norm(sum);
        memcpy(tang + 3 * i, sum, 3 * sizeof(float));
    }
    float seed[3];
    memcpy(seed, initial, 3 * sizeof(float));
    float proj = rv_dot(seed, tang);
    seed[0] -= tang[0] * proj;
    seed[1] -= tang[1] * proj;
    seed[2] -= tang[2] * proj;
    if (!(rv_dot(seed, seed) > 0.0f)) {
        return -1;
    }
    rv_norm(seed);
    memcpy(maj, seed, 3 * sizeof(float));
    for (int i = 1; i < n; i++) {
        if (!rv_transport(maj + 3 * i, maj + 3 * (i - 1),
                tang + 3 * (i - 1), tang + 3 * i)) {
            return -1;
        }
        rv_norm(maj + 3 * i);
    }
    float twist = 0.0f;
    if (closed) {
        float carried[3];
        if (!rv_transport(carried, maj + 3 * (n - 1),
                tang + 3 * (n - 1), tang)) {
            return -1;
        }
        twist = rv_signed_angle(carried, maj, tang);
        for (int i = 0; i < n; i++) {
            float fixed[3];
            rv_rodrigues(fixed, maj + 3 * i, tang + 3 * i,
                twist * cum[i] / total);
            memcpy(maj + 3 * i, fixed, 3 * sizeof(float));
        }
    }
    *total_out = total;
    if (twist_out) {
        *twist_out = twist;
    }
    return 0;
}

static void rv_section_point(float out[3], float const center[3],
    float const t[3], float const m[3], float a, float b, float theta)
{
    float minor[3];
    rv_cross(minor, t, m);
    for (int i = 0; i < 3; i++) {
        out[i] = center[i] + m[i] * (a * cosf(theta))
            + minor[i] * (b * sinf(theta));
    }
}

// Gradient of the elliptical implicit equation, i.e. the true outward normal
// of the continuous section rather than a naive radial vector.
static void rv_section_normal(float out[3], float const t[3],
    float const m[3], float a, float b, float theta)
{
    float minor[3];
    rv_cross(minor, t, m);
    for (int i = 0; i < 3; i++) {
        out[i] = m[i] * (cosf(theta) / a) + minor[i] * (sinf(theta) / b);
    }
    rv_norm(out);
}

// Independent area-weighted accumulation over the actual discrete triangles,
// followed by the same two seam sharings used by the library.
static void rv_mesh_normals(par_shapes_mesh const* mesh, int nring,
    int slices, int closed, float* out)
{
    int rv = slices + 1;
    memset(out, 0, 3 * mesh->npoints * sizeof(float));
    for (int f = 0; f < mesh->ntriangles; f++) {
        PAR_SHAPES_T const* tri = mesh->triangles + 3 * f;
        float const* pa = mesh->points + 3 * tri[0];
        float const* pb = mesh->points + 3 * tri[1];
        float const* pc = mesh->points + 3 * tri[2];
        float e1[3], e2[3], cp[3];
        rv_sub(e1, pb, pa);
        rv_sub(e2, pc, pa);
        rv_cross(cp, e1, e2);
        for (int q = 0; q < 3; q++) {
            out[3 * tri[q] + 0] += cp[0];
            out[3 * tri[q] + 1] += cp[1];
            out[3 * tri[q] + 2] += cp[2];
        }
    }
    for (int r = 0; r < nring; r++) {
        float* seam = out + 3 * (r * rv + slices);
        float* start = out + 3 * r * rv;
        for (int i = 0; i < 3; i++) {
            start[i] += seam[i];
            seam[i] = start[i];
        }
    }
    if (closed) {
        int n = nring - 1;
        for (int k = 0; k <= slices; k++) {
            float* first = out + 3 * k;
            float* last = out + 3 * (n * rv + k);
            for (int i = 0; i < 3; i++) {
                first[i] += last[i];
                last[i] = first[i];
            }
        }
    }
    for (int v = 0; v < mesh->npoints; v++) {
        rv_norm(out + 3 * v);
    }
}

static int rv_close3(float const a[3], float const b[3], float eps)
{
    float d[3];
    rv_sub(d, a, b);
    return rv_dot(d, d) <= eps * eps;
}

static int rv_all_finite(par_shapes_mesh const* m)
{
    int nf = 3 * m->npoints;
    for (int i = 0; i < nf; i++) {
        if (!isfinite(m->points[i]) || !isfinite(m->normals[i])) {
            return 0;
        }
    }
    for (int i = 0; i < 2 * m->npoints; i++) {
        if (!isfinite(m->tcoords[i])) {
            return 0;
        }
    }
    return 1;
}

// -- contract_transport -------------------------------------------------------

int contract_transport(void)
{
    g_checks = 0;
    int slices = 12;
    float centers[7][3] = {
        {0, 0, 0}, {0, 0, 2}, {0, 0, 4}, {0, 0, 6},
        {0, 2, 6}, {2, 2, 6}, {2, 2, 4}
    };
    float axes[7][2];
    for (int i = 0; i < 7; i++) {
        axes[i][0] = 0.4f + 0.08f * i;
        axes[i][1] = 0.2f + 0.05f * i;
    }
    float initial[3] = {1, 0, 0};
    par_shapes_mesh* m = par_shapes_create_sweep(&centers[0][0], 7,
        &axes[0][0], slices, false, initial);
    CHECK(m);
    CHECK(m->points && m->triangles && m->normals && m->tcoords);
    CHECK(m->npoints == 7 * (slices + 1));
    CHECK(m->ntriangles == 2 * 6 * slices);
    CHECK(rv_all_finite(m));

    float tang[21], maj[21], cum[8], total;
    CHECK(rv_build_frames(&centers[0][0], 7, 0, initial, tang, maj, cum,
        &total, 0) == 0);

    // The long straight run must keep a fixed world-space major axis.
    for (int r = 1; r <= 3; r++) {
        CHECK(rv_close3(maj + 3 * r, maj, 1e-6f));
    }
    CHECK(rv_close3(maj, (float[3]){1, 0, 0}, 1e-6f));

    // Every vertex position and texture coordinate matches the reference.
    for (int r = 0; r < 7; r++) {
        for (int k = 0; k <= slices; k++) {
            float theta = 2.0f * (float) PAR_PI * k / slices;
            float expect[3];
            rv_section_point(expect, centers[r], tang + 3 * r, maj + 3 * r,
                axes[r][0], axes[r][1], theta);
            int vert = r * (slices + 1) + k;
            CHECK(rv_close3(m->points + 3 * vert, expect, 2e-4f));
            float const* uv = m->tcoords + 2 * vert;
            CHECK(fabsf(uv[0] - cum[r] / total) < 1e-6f);
            CHECK(fabsf(uv[1] - (float) k / slices) < 1e-6f);
        }
    }

    // Angular seam: duplicated position, shared normal, split texture coord.
    for (int r = 0; r < 7; r++) {
        int a = r * (slices + 1);
        int b = a + slices;
        CHECK(rv_close3(m->points + 3 * a, m->points + 3 * b, 1e-6f));
        CHECK(rv_close3(m->normals + 3 * a, m->normals + 3 * b, 1e-6f));
        CHECK(fabsf(m->tcoords[2 * a + 1]) < 1e-6f);
        CHECK(fabsf(m->tcoords[2 * b + 1] - 1.0f) < 1e-6f);
    }

    // Every triangle winds toward the outward side of its section.
    for (int r = 0; r < 6; r++) {
        for (int k = 0; k < slices; k++) {
            int ia = r * (slices + 1) + k;
            PAR_SHAPES_T const* tri = 0;
            for (int f = 0; f < m->ntriangles; f++) {
                if (m->triangles[3 * f] == (PAR_SHAPES_T) ia) {
                    tri = m->triangles + 3 * f;
                    break;
                }
            }
            CHECK(tri);
            float e1[3], e2[3], area[3];
            rv_sub(e1, m->points + 3 * tri[1], m->points + 3 * tri[0]);
            rv_sub(e2, m->points + 3 * tri[2], m->points + 3 * tri[0]);
            rv_cross(area, e1, e2);
            float theta = 2.0f * (float) PAR_PI * k / slices;
            float outward[3];
            rv_section_normal(outward, tang + 3 * r, maj + 3 * r,
                axes[r][0], axes[r][1], theta);
            CHECK(rv_dot(area, outward) > 0.0f);
        }
    }

    // Normals come from discrete area weighting, including seam sharing.
    float* refn = (float*) malloc(3 * m->npoints * sizeof(float));
    CHECK(refn);
    rv_mesh_normals(m, 7, slices, 0, refn);
    for (int i = 0; i < m->npoints; i++) {
        CHECK(rv_close3(m->normals + 3 * i, refn + 3 * i, 2e-3f));
        float n[3];
        memcpy(n, m->normals + 3 * i, 3 * sizeof(float));
        CHECK(fabsf(rv_dot(n, n) - 1.0f) < 1e-4f);
    }
    free(refn);

    // The produced arrays work with the existing clone, merge and transform
    // entry points.
    par_shapes_mesh* copy = par_shapes_clone(m, 0);
    CHECK(copy && copy->npoints == m->npoints);
    CHECK(copy->ntriangles == m->ntriangles);
    par_shapes_mesh* scene = par_shapes_create_empty();
    par_shapes_merge(scene, m);
    par_shapes_merge_and_free(scene, copy);
    CHECK(scene->npoints == 2 * m->npoints);
    CHECK(scene->ntriangles == 2 * m->ntriangles);
    float before[3];
    memcpy(before, scene->points, 3 * sizeof(float));
    par_shapes_translate(scene, 1.0f, 2.0f, 3.0f);
    CHECK(fabsf(scene->points[0] - (before[0] + 1.0f)) < 1e-6f);
    CHECK(fabsf(scene->points[1] - (before[1] + 2.0f)) < 1e-6f);
    CHECK(fabsf(scene->points[2] - (before[2] + 3.0f)) < 1e-6f);
    par_shapes_free_mesh(scene);
    par_shapes_free_mesh(m);
    return g_checks;
}

// -- contract_closure ---------------------------------------------------------

int contract_closure(void)
{
    g_checks = 0;
    int slices = 10;
    int n = 6;
    float centers[6][3] = {
        {2, 0, 0}, {0, 2, 1}, {-2, 0, 0}, {0, -2, 1},
        {1, -1, -1}, {-1, 1, -1}
    };
    float axes[6][2];
    for (int i = 0; i < n; i++) {
        axes[i][0] = 0.5f;
        axes[i][1] = 0.3f;
    }
    float initial[3] = {0, 0, 1};
    par_shapes_mesh* m = par_shapes_create_sweep(&centers[0][0], n,
        &axes[0][0], slices, true, initial);
    CHECK(m);
    int nring = n + 1;
    int rv = slices + 1;
    CHECK(m->npoints == nring * rv);
    CHECK(m->ntriangles == 2 * n * slices);
    CHECK(rv_all_finite(m));

    float tang[18], maj[18], cum[7], total, twist;
    CHECK(rv_build_frames(&centers[0][0], n, 1, initial, tang, maj, cum,
        &total, &twist) == 0);
    CHECK(fabsf(twist) > 1e-3f);

    for (int r = 0; r < nring; r++) {
        int pi = r % n;
        for (int k = 0; k <= slices; k++) {
            float theta = 2.0f * (float) PAR_PI * k / slices;
            float expect[3];
            rv_section_point(expect, centers[pi], tang + 3 * pi,
                maj + 3 * pi, axes[pi][0], axes[pi][1], theta);
            int vert = r * rv + k;
            CHECK(rv_close3(m->points + 3 * vert, expect, 2e-4f));
            float const* uv = m->tcoords + 2 * vert;
            float want_u = (r == n) ? 1.0f : cum[r] / total;
            CHECK(fabsf(uv[0] - want_u) < 1e-6f);
            CHECK(fabsf(uv[1] - (float) k / slices) < 1e-6f);
        }
    }

    // Closure seam: the duplicated first ring shares geometry and normals.
    for (int k = 0; k <= slices; k++) {
        CHECK(rv_close3(m->points + 3 * k,
            m->points + 3 * (n * rv + k), 1e-6f));
        CHECK(rv_close3(m->normals + 3 * k,
            m->normals + 3 * (n * rv + k), 1e-6f));
        CHECK(fabsf(m->tcoords[2 * k] - 0.0f) < 1e-6f);
        CHECK(fabsf(m->tcoords[2 * (n * rv + k)] - 1.0f) < 1e-6f);
    }
    // Angular seam on every ring.
    for (int r = 0; r < nring; r++) {
        CHECK(rv_close3(m->normals + 3 * (r * rv),
            m->normals + 3 * (r * rv + slices), 1e-6f));
    }

    // The correction is amortized along arc length: an interior ring deviates
    // from the raw transported frame by twist * cum / total, and the final
    // ring closes the gap completely instead of snapping at the last edge.
    float raw[18];
    memcpy(raw, maj, 3 * sizeof(float));
    for (int i = 1; i < n; i++) {
        CHECK(rv_transport(raw + 3 * i, raw + 3 * (i - 1),
            tang + 3 * (i - 1), tang + 3 * i));
        rv_norm(raw + 3 * i);
    }
    int probe = 2;
    float spread = rv_signed_angle(raw + 3 * probe, maj + 3 * probe,
        tang + 3 * probe);
    CHECK(fabsf(spread - twist * cum[probe] / total) < 1e-4f);
    CHECK(fabsf(spread) > 1e-4f);

    // Reference normals over the closed surface, both seams included.
    float* refn = (float*) malloc(3 * m->npoints * sizeof(float));
    CHECK(refn);
    rv_mesh_normals(m, nring, slices, 1, refn);
    for (int i = 0; i < m->npoints; i++) {
        CHECK(rv_close3(m->normals + 3 * i, refn + 3 * i, 2e-3f));
    }
    free(refn);

    // All triangles still point outward after the twist redistribution.
    for (int f = 0; f < m->ntriangles; f++) {
        PAR_SHAPES_T const* tri = m->triangles + 3 * f;
        int ring = tri[0] / rv;
        int k = tri[0] % rv;
        int pi = ring % n;
        float e1[3], e2[3], area[3];
        rv_sub(e1, m->points + 3 * tri[1], m->points + 3 * tri[0]);
        rv_sub(e2, m->points + 3 * tri[2], m->points + 3 * tri[0]);
        rv_cross(area, e1, e2);
        float theta = 2.0f * (float) PAR_PI * k / slices;
        float outward[3];
        rv_section_normal(outward, tang + 3 * pi, maj + 3 * pi,
            axes[pi][0], axes[pi][1], theta);
        CHECK(rv_dot(area, outward) > 0.0f);
    }
    par_shapes_free_mesh(m);

    // Non-planar loop whose parallel-transport residual sits just above the
    // (-pi, pi] branch cut; the principal angle must resolve to the negative
    // value rather than its positive wrap, and the mesh must still close.
    float hc[5][3] = {
        {0.942405f, -0.991287f, -0.553777f},
        {0.674881f, 0.717171f, -0.513762f},
        {-0.795802f, 0.960734f, -0.939945f},
        {0.337839f, 0.057781f, -0.869831f},
        {0.756778f, -0.826334f, -0.202011f}
    };
    float ha[5][2] = {
        {0.12f, 0.07f}, {0.12f, 0.07f}, {0.12f, 0.07f},
        {0.12f, 0.07f}, {0.12f, 0.07f}
    };
    float hd[3] = {0.805835f, -0.338134f, 0.988268f};
    par_shapes_mesh* hm = par_shapes_create_sweep(&hc[0][0], 5, &ha[0][0],
        12, true, hd);
    CHECK(hm);
    float ht[15], hmj[15], hcum[6], htot, htwist;
    CHECK(rv_build_frames(&hc[0][0], 5, 1, hd, ht, hmj, hcum, &htot,
        &htwist) == 0);
    CHECK(htwist < -((float) PAR_PI - 0.001f));
    CHECK(htwist > -(float) PAR_PI);
    for (int k = 0; k <= 12; k++) {
        CHECK(rv_close3(hm->points + 3 * k,
            hm->points + 3 * (5 * 13 + k), 1e-5f));
        CHECK(rv_close3(hm->normals + 3 * k,
            hm->normals + 3 * (5 * 13 + k), 1e-5f));
    }
    par_shapes_free_mesh(hm);

    // Exact half-turn convention: an antiparallel transported frame has zero
    // signed sine and a negative cosine, and the documented rule resolves the
    // branch cut to positive pi rather than negative pi.
    float exact_s = 0.0f;
    float exact_c = -1.0f;
    float exact_ang = atan2f(exact_s, exact_c);
    CHECK(exact_ang > 0.0f);
    CHECK(fabsf(exact_ang - (float) PAR_PI) < 1e-6f);
    CHECK(atan2f(-0.0f, exact_c) < 0.0f);
    return g_checks;
}

// -- contract_surface ---------------------------------------------------------

int contract_surface(void)
{
    g_checks = 0;
    int slices = 8;
    int n = 4;
    float centers[4][3] = {
        {0, 0, 0}, {1, 0, 0}, {2, 0.5f, 0.3f}, {3, 0.5f, 1.3f}
    };
    float axes[4][2] = {
        {0.8f, 0.4f}, {0.7f, 0.35f}, {0.6f, 0.3f}, {0.5f, 0.25f}
    };
    float initial[3] = {0, 1, 0};
    par_shapes_mesh* m = par_shapes_create_sweep(&centers[0][0], n,
        &axes[0][0], slices, false, initial);
    CHECK(m);
    CHECK(rv_all_finite(m));

    float tang[12], maj[12], cum[5], total;
    CHECK(rv_build_frames(&centers[0][0], n, 0, initial, tang, maj, cum,
        &total, 0) == 0);

    // Discrete area-weighted normals match an independent accumulation.
    float* refn = (float*) malloc(3 * m->npoints * sizeof(float));
    CHECK(refn);
    rv_mesh_normals(m, n, slices, 0, refn);
    for (int i = 0; i < m->npoints; i++) {
        CHECK(rv_close3(m->normals + 3 * i, refn + 3 * i, 2e-3f));
    }
    free(refn);

    // Elliptical normals are not radial: at 45 degrees the gradient normal
    // tilts away from the center-to-vertex direction.
    int probe_k = 1;
    int vert = 2 * (slices + 1) + probe_k;
    float radial[3];
    rv_sub(radial, m->points + 3 * vert, centers[2]);
    rv_norm(radial);
    CHECK(rv_dot(radial, m->normals + 3 * vert) < 0.95f);

    // No degenerate triangles.
    for (int f = 0; f < m->ntriangles; f++) {
        PAR_SHAPES_T const* tri = m->triangles + 3 * f;
        float e1[3], e2[3], area[3];
        rv_sub(e1, m->points + 3 * tri[1], m->points + 3 * tri[0]);
        rv_sub(e2, m->points + 3 * tri[2], m->points + 3 * tri[0]);
        rv_cross(area, e1, e2);
        CHECK(rv_dot(area, area) > 1e-12f);
    }

    // Long straight elliptical pipe feeding a turn: seam normals stay
    // continuous and the straight section lights along the ellipse gradient,
    // which is distinct from the radial direction off the principal axes.
    int slices2 = 16;
    float straight[5][3] = {
        {0, 0, 0}, {0, 0, 1}, {0, 0, 2}, {0, 0, 3}, {1, 0, 3}
    };
    float saxes[5][2] = {
        {0.6f, 0.35f}, {0.6f, 0.35f}, {0.6f, 0.35f}, {0.6f, 0.35f},
        {0.6f, 0.35f}
    };
    float sinit[3] = {1, 0, 0};
    par_shapes_mesh* s = par_shapes_create_sweep(&straight[0][0], 5,
        &saxes[0][0], slices2, false, sinit);
    CHECK(s);
    for (int r = 0; r < 5; r++) {
        CHECK(rv_close3(s->normals + 3 * r * (slices2 + 1),
            s->normals + 3 * (r * (slices2 + 1) + slices2), 1e-6f));
    }
    float zaxis[3] = {0, 0, 1};
    float xaxis[3] = {1, 0, 0};
    float side[3];
    rv_section_normal(side, zaxis, xaxis, 0.6f, 0.35f, 0.0f);
    CHECK(rv_close3(side, s->normals + 3 * 1 * (slices2 + 1), 2e-3f));
    int angled = 2;
    float analytic[3];
    rv_section_normal(analytic, zaxis, xaxis, 0.6f, 0.35f,
        2.0f * (float) PAR_PI * angled / slices2);
    float arad[3];
    rv_sub(arad, s->points + 3 * (1 * (slices2 + 1) + angled),
        straight[1]);
    rv_norm(arad);
    CHECK(rv_dot(arad, analytic) < 0.97f);
    CHECK(rv_close3(analytic,
        s->normals + 3 * (1 * (slices2 + 1) + angled), 2e-3f));
    par_shapes_free_mesh(s);
    par_shapes_free_mesh(m);
    return g_checks;
}

// -- contract_limits ----------------------------------------------------------

int contract_limits(void)
{
    g_checks = 0;
    float good[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    float gaxes[3][2] = {{0.5f, 0.25f}, {0.5f, 0.25f}, {0.5f, 0.25f}};
    float init[3] = {0, 1, 0};

    CHECK(!par_shapes_create_sweep(0, 3, &gaxes[0][0], 8, false, init));
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, 0, 8, false, init));
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &gaxes[0][0], 8,
        false, 0));
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &gaxes[0][0], 2,
        false, init));
    CHECK(!par_shapes_create_sweep(&good[0][0], 1, &gaxes[0][0], 8,
        false, init));
    CHECK(!par_shapes_create_sweep(&good[0][0], 2, &gaxes[0][0], 8,
        true, init));

    float bad_nan[3][3] = {{0, 0, 0}, {1, NAN, 0}, {1, 1, 0}};
    CHECK(!par_shapes_create_sweep(&bad_nan[0][0], 3, &gaxes[0][0], 8,
        false, init));
    float bad_inf[3][3] = {{0, 0, 0}, {1, INFINITY, 0}, {1, 1, 0}};
    CHECK(!par_shapes_create_sweep(&bad_inf[0][0], 3, &gaxes[0][0], 8,
        false, init));
    float zero_axis[3][2] = {{0.5f, 0.25f}, {0.0f, 0.25f}, {0.5f, 0.25f}};
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &zero_axis[0][0], 8,
        false, init));
    float neg_axis[3][2] = {{0.5f, 0.25f}, {0.5f, -0.25f},
        {0.5f, 0.25f}};
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &neg_axis[0][0], 8,
        false, init));

    float coincide[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    CHECK(!par_shapes_create_sweep(&coincide[0][0], 3, &gaxes[0][0], 8,
        false, init));

    float reverse_open[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 0}};
    CHECK(!par_shapes_create_sweep(&reverse_open[0][0], 3, &gaxes[0][0],
        8, false, init));
    float reverse_closed[3][3] = {{0, 0, 0}, {1, 0, 0}, {0.5f, 0, 0}};
    CHECK(!par_shapes_create_sweep(&reverse_closed[0][0], 3,
        &gaxes[0][0], 8, true, init));

    float parallel[3] = {1, 0, 0};
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &gaxes[0][0], 8,
        false, parallel));
    float zero_dir[3] = {0, 0, 0};
    CHECK(!par_shapes_create_sweep(&good[0][0], 3, &gaxes[0][0], 8,
        false, zero_dir));

    // Failed calls never modify their inputs.
    float centers_snap[9], axes_snap[6];
    memcpy(centers_snap, &coincide[0][0], sizeof(centers_snap));
    memcpy(axes_snap, &gaxes[0][0], sizeof(axes_snap));
    CHECK(!par_shapes_create_sweep(&coincide[0][0], 3, &gaxes[0][0], 8,
        false, init));
    CHECK(!memcmp(centers_snap, &coincide[0][0], sizeof(centers_snap)));
    CHECK(!memcmp(axes_snap, &gaxes[0][0], sizeof(axes_snap)));

    // Default indexing stays 16-bit and overflows are rejected up front.
    PAR_SHAPES_T cap = (PAR_SHAPES_T)(-1);
    CHECK(cap == 65535);
    int big = 4000;
    float* bigc = (float*) calloc(3 * big, sizeof(float));
    float* biga = (float*) calloc(2 * big, sizeof(float));
    CHECK(bigc && biga);
    for (int i = 0; i < big; i++) {
        bigc[3 * i] = (float) i;
        bigc[3 * i + 1] = (i % 2) ? 1.0f : 0.0f;
        biga[2 * i] = 0.3f;
        biga[2 * i + 1] = 0.2f;
    }
    CHECK(!par_shapes_create_sweep(bigc, big, biga, 20, false, init));
    CHECK(!par_shapes_create_sweep(bigc, 6000, biga, 20, true, init));

    // A mesh just under the index cap succeeds with valid indices.
    int okay = 3000;
    par_shapes_mesh* m = par_shapes_create_sweep(bigc, okay, biga, 19,
        false, init);
    CHECK(m);
    CHECK(m->npoints == okay * 20);
    for (int i = 0; i < 3 * m->ntriangles; i++) {
        CHECK(m->triangles[i] < cap);
    }
    par_shapes_free_mesh(m);
    free(bigc);
    free(biga);

    // Absurd counts fail the product checks before any array is touched.
    float dummy_c[3] = {0, 0, 0};
    float dummy_a[2] = {0.5f, 0.25f};
    CHECK(!par_shapes_create_sweep(dummy_c, 0x70000000, dummy_a, 3,
        false, init));
    CHECK(!par_shapes_create_sweep(dummy_c, 8, dummy_a, 0x70000000,
        false, init));
    return g_checks;
}
