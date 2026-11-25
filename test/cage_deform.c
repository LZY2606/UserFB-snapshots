// Cage deformation tests for the mean value coordinate API in par_shapes.h.
//
// The independent central differences in this file exist only in the test;
// the library itself produces gradients analytically via automatic
// differentiation.

#include "par_shapes.h"
#include "../task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int near_eq(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static double vmax3(const double* a, const double* b)
{
    double e = fabs(a[0] - b[0]);
    if (fabs(a[1] - b[1]) > e) e = fabs(a[1] - b[1]);
    if (fabs(a[2] - b[2]) > e) e = fabs(a[2] - b[2]);
    return e;
}

// A one-point mesh still needs an index buffer for the binding contract.
static par_shapes_mesh* point_mesh(double x, double y, double z)
{
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = 1;
    m->ntriangles = 1;
    m->points = PAR_MALLOC(float, 3);
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 3);
    m->points[0] = (float) x;
    m->points[1] = (float) y;
    m->points[2] = (float) z;
    m->triangles[0] = m->triangles[1] = m->triangles[2] = 0;
    return m;
}

// Non-convex cage: a subdivided sphere whose upper ring is pushed hard
// towards the axis, folding a band inward.  It stays closed and manifold but
// develops genuine negative mean value contributions at interior points near
// the fold, while face orientations are generic (no axis-aligned planes).
static par_shapes_mesh* fold_cage(void)
{
    par_shapes_mesh* cage = par_shapes_create_subdivided_sphere(2);
    for (int i = 0; i < cage->npoints; i++) {
        double z = cage->points[3 * i + 2];
        if (z > 0.45 && z < 0.85) {
            cage->points[3 * i + 0] *= 0.4f;
            cage->points[3 * i + 1] *= 0.4f;
            cage->points[3 * i + 2] *= 0.4f;
        }
    }
    return cage;
}

// Fine points strictly inside the folded cage.  The first two carry negative
// weights; the rest are well-conditioned points used for differential checks.
static const double FOLD_POINTS[][3] = {
    { -0.4663, -0.1509, 0.0673 },
    { -0.3463,  0.2091, 0.1873 },
    {  0.0000,  0.0000, 0.0000 },
    {  0.0500,  0.0500, 0.0500 },
    {  0.2000,  0.1000, -0.3000 }
};
#define FOLD_COUNT 5
#define FOLD_FIRST_NEG 0
#define FOLD_LAST_NEG 1
#define FOLD_FD_A 2
#define FOLD_FD_B 3

static par_shapes_mesh* fold_fine_mesh(void)
{
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = FOLD_COUNT;
    m->ntriangles = FOLD_COUNT;
    m->points = PAR_MALLOC(float, 3 * FOLD_COUNT);
    m->normals = PAR_MALLOC(float, 3 * FOLD_COUNT);
    m->tcoords = PAR_MALLOC(float, 2 * FOLD_COUNT);
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 3 * FOLD_COUNT);
    for (int i = 0; i < FOLD_COUNT; i++) {
        for (int k = 0; k < 3; k++) {
            m->points[3 * i + k] = (float) FOLD_POINTS[i][k];
        }
        double n[3] = { 0.3 * i + 0.1, 0.7 - 0.2 * i, 0.5 };
        double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        m->normals[3 * i + 0] = (float)(n[0] / l);
        m->normals[3 * i + 1] = (float)(n[1] / l);
        m->normals[3 * i + 2] = (float)(n[2] / l);
        m->tcoords[2 * i + 0] = (float) i * 0.25f;
        m->tcoords[2 * i + 1] = 1.0f - (float) i * 0.125f;
        m->triangles[3 * i + 0] = (PAR_SHAPES_T) i;
        m->triangles[3 * i + 1] = (PAR_SHAPES_T) i;
        m->triangles[3 * i + 2] = (PAR_SHAPES_T) i;
    }
    return m;
}

// Verify every coordinate identity for one point of a binding.
static int check_identities(const par_shapes_cage_binding* b,
    const par_shapes_mesh* cage, int point, const double* x)
{
    int n = cage->npoints;
    double* w = PAR_MALLOC(double, n);
    double* g = PAR_MALLOC(double, 3 * n);
    TASK_CHECK(par_shapes_cage_coordinates(b, point, w, g));

    double sum_w = 0.0;
    for (int i = 0; i < n; i++) sum_w += w[i];
    TASK_CHECK(near_eq(sum_w, 1.0, 1e-9));

    double sum_g[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) sum_g[k] += g[3 * i + k];
    }
    for (int k = 0; k < 3; k++) TASK_CHECK(fabs(sum_g[k]) <= 1e-8);

    // Linear reproduction: sum w_i c_i = x.
    double reproduced[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            reproduced[k] += w[i] * cage->points[3 * i + k];
        }
    }
    TASK_CHECK(vmax3(reproduced, x) <= 1e-7);

    // First order identity: sum c_i (x) grad(w_i) = I.
    for (int a = 0; a < 3; a++) {
        for (int c = 0; c < 3; c++) {
            double s = 0.0;
            for (int i = 0; i < n; i++) {
                s += (double) cage->points[3 * i + a] * g[3 * i + c];
            }
            double want = (a == c) ? 1.0 : 0.0;
            TASK_CHECK(fabs(s - want) <= 1e-7);
        }
    }
    PAR_FREE(w);
    PAR_FREE(g);
    return 0;
}

// ---------------------------------------------------------------------------
// coordinates group
// ---------------------------------------------------------------------------

static int case_tetra_coordinates(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double centroid[3] = { 0, 0, 0 };
    for (int i = 0; i < cage->npoints; i++) {
        for (int k = 0; k < 3; k++) {
            centroid[k] += cage->points[3 * i + k];
        }
    }
    for (int k = 0; k < 3; k++) centroid[k] /= cage->npoints;

    par_shapes_mesh* fine = point_mesh(centroid[0], centroid[1], centroid[2]);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    TASK_CHECK(check_identities(b, cage, 0, centroid) == 0);

    // A convex cage gives non-negative mean value weights.
    double w[64];
    TASK_CHECK(cage->npoints <= 64);
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w, 0));
    for (int i = 0; i < cage->npoints; i++) TASK_CHECK(w[i] >= -1e-12);

    // A globally reversed winding must not change any coordinate.
    par_shapes_mesh* reversed = par_shapes_clone(cage, 0);
    par_shapes_invert(reversed, 0, 0);
    par_shapes_cage_binding* br = par_shapes_cage_bind(reversed, fine);
    TASK_CHECK(br != 0);
    double wr[64], gr[192], g0[192];
    TASK_CHECK(par_shapes_cage_coordinates(br, 0, wr, gr));
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, 0, g0));
    for (int i = 0; i < cage->npoints; i++) {
        TASK_CHECK(fabs(wr[i] - w[i]) <= 1e-12);
        for (int k = 0; k < 3; k++) {
            TASK_CHECK(fabs(gr[3 * i + k] - g0[3 * i + k]) <= 1e-10);
        }
    }

    // Out-of-range point indices are rejected rather than read blindly.
    TASK_CHECK(!par_shapes_cage_coordinates(b, 7, 0, 0));
    TASK_CHECK(!par_shapes_cage_coordinates(b, -1, 0, 0));

    par_shapes_cage_free(br);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(reversed);
    par_shapes_free_mesh(fine);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_nonconvex_signed_coordinates(void)
{
    par_shapes_mesh* cage = fold_cage();
    par_shapes_mesh* fine = fold_fine_mesh();
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    int saw_negative = 0;
    for (int p = 0; p < FOLD_COUNT; p++) {
        double x[3];
        for (int k = 0; k < 3; k++) x[k] = fine->points[3 * p + k];
        TASK_CHECK(check_identities(b, cage, p, x) == 0);
        double* w = PAR_MALLOC(double, cage->npoints);
        TASK_CHECK(par_shapes_cage_coordinates(b, p, w, 0));
        for (int i = 0; i < cage->npoints; i++) {
            if (w[i] < -1e-9) saw_negative = 1;
        }
        PAR_FREE(w);
    }
    // The folded region must produce signed (negative) contributions.
    TASK_CHECK(saw_negative);

    // Scaling cage and fine mesh together preserves weights and makes the
    // gradients shrink by the inverse length factor.
    const double scale = 2.5;
    par_shapes_mesh* big_cage = par_shapes_clone(cage, 0);
    par_shapes_scale(big_cage, (float) scale, (float) scale, (float) scale);
    par_shapes_mesh* big_fine = par_shapes_clone(fine, 0);
    par_shapes_scale(big_fine, (float) scale, (float) scale, (float) scale);
    par_shapes_cage_binding* bb = par_shapes_cage_bind(big_cage, big_fine);
    TASK_CHECK(bb != 0);
    for (int p = 0; p < FOLD_COUNT; p++) {
        double* w1 = PAR_MALLOC(double, cage->npoints);
        double* w2 = PAR_MALLOC(double, cage->npoints);
        double* g1 = PAR_MALLOC(double, 3 * cage->npoints);
        double* g2 = PAR_MALLOC(double, 3 * cage->npoints);
        TASK_CHECK(par_shapes_cage_coordinates(b, p, w1, g1));
        TASK_CHECK(par_shapes_cage_coordinates(bb, p, w2, g2));
        for (int i = 0; i < cage->npoints; i++) {
            // Tolerances absorb the float round trip of storing both scaled
            // meshes; the underlying double-precision invariance is exact.
            TASK_CHECK(fabs(w2[i] - w1[i]) <= 1e-6);
            for (int k = 0; k < 3; k++) {
                TASK_CHECK(near_eq(g2[3 * i + k], g1[3 * i + k] / scale,
                    5e-6));
            }
        }
        PAR_FREE(w1);
        PAR_FREE(w2);
        PAR_FREE(g1);
        PAR_FREE(g2);
    }

    par_shapes_cage_free(bb);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(big_fine);
    par_shapes_free_mesh(big_cage);
    par_shapes_free_mesh(fine);
    par_shapes_free_mesh(cage);
    return 0;
}

// ---------------------------------------------------------------------------
// differential group
// ---------------------------------------------------------------------------

// Independent central differences of the weights, using fresh bindings at the
// perturbed positions.  This finite-difference code is test-only.  The step is
// chosen per cage so that float32 quantization of the stored point does not
// dominate the difference; both steps are still true central differences.
static int case_gradient_central_differences(void)
{
    // Convex tetrahedron.
    const double h_tetra = 1e-2;
    par_shapes_mesh* tetra = par_shapes_create_tetrahedron();
    {
        double x[3] = { 0.1, 0.05, -0.07 };
        par_shapes_mesh* f0 = point_mesh(x[0], x[1], x[2]);
        par_shapes_cage_binding* b0 = par_shapes_cage_bind(tetra, f0);
        TASK_CHECK(b0 != 0);
        double w0[64], g0[192];
        TASK_CHECK(par_shapes_cage_coordinates(b0, 0, w0, g0));
        for (int axis = 0; axis < 3; axis++) {
            double xp[3] = { x[0], x[1], x[2] };
            double xm[3] = { x[0], x[1], x[2] };
            xp[axis] += h_tetra;
            xm[axis] -= h_tetra;
            par_shapes_mesh* fp = point_mesh(xp[0], xp[1], xp[2]);
            par_shapes_mesh* fm = point_mesh(xm[0], xm[1], xm[2]);
            par_shapes_cage_binding* bp =
                par_shapes_cage_bind(tetra, fp);
            par_shapes_cage_binding* bm =
                par_shapes_cage_bind(tetra, fm);
            TASK_CHECK(bp != 0 && bm != 0);
            double wp[64], wm[64];
            TASK_CHECK(par_shapes_cage_coordinates(bp, 0, wp, 0));
            TASK_CHECK(par_shapes_cage_coordinates(bm, 0, wm, 0));
            for (int i = 0; i < tetra->npoints; i++) {
                double cd = (wp[i] - wm[i]) / (2.0 * h_tetra);
                TASK_CHECK(fabs(cd - g0[3 * i + axis]) <= 1e-5);
            }
            par_shapes_cage_free(bp);
            par_shapes_cage_free(bm);
            par_shapes_free_mesh(fp);
            par_shapes_free_mesh(fm);
        }
        par_shapes_cage_free(b0);
        par_shapes_free_mesh(f0);
    }

    // Non-convex folded cage, evaluated at well-conditioned interior points.
    const double h_fold = 1e-3;
    par_shapes_mesh* cage = fold_cage();
    int healthy[2] = { FOLD_FD_A, FOLD_FD_B };
    for (int hp = 0; hp < 2; hp++) {
        const double* x = FOLD_POINTS[healthy[hp]];
        par_shapes_mesh* f0 = point_mesh(x[0], x[1], x[2]);
        par_shapes_cage_binding* b0 = par_shapes_cage_bind(cage, f0);
        TASK_CHECK(b0 != 0);
        double* w0 = PAR_MALLOC(double, cage->npoints);
        double* g0 = PAR_MALLOC(double, 3 * cage->npoints);
        TASK_CHECK(par_shapes_cage_coordinates(b0, 0, w0, g0));
        for (int axis = 0; axis < 3; axis++) {
            double xp[3] = { x[0], x[1], x[2] };
            double xm[3] = { x[0], x[1], x[2] };
            xp[axis] += h_fold;
            xm[axis] -= h_fold;
            par_shapes_mesh* fp = point_mesh(xp[0], xp[1], xp[2]);
            par_shapes_mesh* fm = point_mesh(xm[0], xm[1], xm[2]);
            par_shapes_cage_binding* bp =
                par_shapes_cage_bind(cage, fp);
            par_shapes_cage_binding* bm =
                par_shapes_cage_bind(cage, fm);
            TASK_CHECK(bp != 0 && bm != 0);
            double* wp = PAR_MALLOC(double, cage->npoints);
            double* wm = PAR_MALLOC(double, cage->npoints);
            TASK_CHECK(par_shapes_cage_coordinates(bp, 0, wp, 0));
            TASK_CHECK(par_shapes_cage_coordinates(bm, 0, wm, 0));
            for (int i = 0; i < cage->npoints; i++) {
                double cd = (wp[i] - wm[i]) / (2.0 * h_fold);
                TASK_CHECK(fabs(cd - g0[3 * i + axis]) <= 1e-5);
            }
            PAR_FREE(wp);
            PAR_FREE(wm);
            par_shapes_cage_free(bp);
            par_shapes_cage_free(bm);
            par_shapes_free_mesh(fp);
            par_shapes_free_mesh(fm);
        }
        PAR_FREE(w0);
        PAR_FREE(g0);
        par_shapes_cage_free(b0);
        par_shapes_free_mesh(f0);
    }

    par_shapes_free_mesh(cage);
    par_shapes_free_mesh(tetra);
    return 0;
}

static void mat_mul(const double A[3][3], const double* v, double* out)
{
    for (int i = 0; i < 3; i++) {
        out[i] = A[i][0] * v[0] + A[i][1] * v[1] + A[i][2] * v[2];
    }
}

// Estimate the deformation Jacobian at a rest point with central differences
// over freshly bound single-point meshes.
static int cd_jacobian(par_shapes_mesh* cage, const double* x,
    const double* pose, double h, double jac[3][3])
{
    for (int axis = 0; axis < 3; axis++) {
        double xp[3] = { x[0], x[1], x[2] };
        double xm[3] = { x[0], x[1], x[2] };
        xp[axis] += h;
        xm[axis] -= h;
        par_shapes_mesh* fp = point_mesh(xp[0], xp[1], xp[2]);
        par_shapes_mesh* fm = point_mesh(xm[0], xm[1], xm[2]);
        par_shapes_cage_binding* bp = par_shapes_cage_bind(cage, fp);
        par_shapes_cage_binding* bm = par_shapes_cage_bind(cage, fm);
        TASK_CHECK(bp != 0 && bm != 0);
        par_shapes_mesh *op = 0, *om = 0;
        TASK_CHECK(par_shapes_cage_apply(bp, pose, &op));
        TASK_CHECK(par_shapes_cage_apply(bm, pose, &om));
        for (int r = 0; r < 3; r++) {
            jac[r][axis] =
                ((double) op->points[r] - (double) om->points[r]) /
                (2.0 * h);
        }
        par_shapes_free_mesh(op);
        par_shapes_free_mesh(om);
        par_shapes_cage_free(bp);
        par_shapes_cage_free(bm);
        par_shapes_free_mesh(fp);
        par_shapes_free_mesh(fm);
    }
    return 0;
}

static double mat_diff(const double A[3][3], const double B[3][3])
{
    double e = 0.0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (fabs(A[i][j] - B[i][j]) > e) e = fabs(A[i][j] - B[i][j]);
        }
    }
    return e;
}

static int case_nonaffine_jacobian(void)
{
    par_shapes_mesh* cage = fold_cage();
    par_shapes_mesh* fine = fold_fine_mesh();
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    // A nonlinear pose: drag one cage vertex far from its rest position.
    double* pose = PAR_MALLOC(double, 3 * cage->npoints);
    for (int i = 0; i < cage->npoints; i++) {
        for (int k = 0; k < 3; k++) {
            pose[3 * i + k] = cage->points[3 * i + k];
        }
    }
    int moved = cage->npoints / 2;
    pose[3 * moved + 0] += 0.35;
    pose[3 * moved + 2] -= 0.25;

    par_shapes_mesh* deformed = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &deformed));
    TASK_CHECK(deformed != 0);

    // Per-point Jacobians must differ between two rest locations.
    double j1[3][3], j2[3][3];
    TASK_CHECK(cd_jacobian(cage, FOLD_POINTS[FOLD_FD_A], pose, 1e-5, j1)
        == 0);
    TASK_CHECK(cd_jacobian(cage, FOLD_POINTS[FOLD_FD_B], pose, 1e-5, j2)
        == 0);
    TASK_CHECK(mat_diff(j1, j2) > 1e-4);

    // Library normals must agree with the cofactor of the CD Jacobian.  Both
    // the analytic normal and the finite-difference Jacobian refer to the same
    // well-conditioned fine point (FOLD_FD_A).
    double cof[3][3];
    double det =
        j1[0][0] * (j1[1][1] * j1[2][2] - j1[1][2] * j1[2][1])
      - j1[0][1] * (j1[1][0] * j1[2][2] - j1[1][2] * j1[2][0])
      + j1[0][2] * (j1[1][0] * j1[2][1] - j1[1][1] * j1[2][0]);
    TASK_CHECK(isfinite(det) && det != 0.0);
    for (int r = 0; r < 3; r++) {
        int r1 = (r + 1) % 3;
        int r2 = (r + 2) % 3;
        cof[r][0] = j1[r1][1] * j1[r2][2] - j1[r1][2] * j1[r2][1];
        cof[r][1] = j1[r1][2] * j1[r2][0] - j1[r1][0] * j1[r2][2];
        cof[r][2] = j1[r1][0] * j1[r2][1] - j1[r1][1] * j1[r2][0];
    }
    int np = FOLD_FD_A;
    double rest_n[3] = { fine->normals[3*np+0], fine->normals[3*np+1],
        fine->normals[3*np+2] };
    double expected[3];
    mat_mul(cof, rest_n, expected);
    double el = sqrt(expected[0] * expected[0] +
        expected[1] * expected[1] + expected[2] * expected[2]);
    TASK_CHECK(el > 0.0);
    for (int k = 0; k < 3; k++) {
        TASK_CHECK(fabs(deformed->normals[3*np+k] - expected[k] / el)
            <= 1e-4);
    }

    PAR_FREE(pose);
    par_shapes_free_mesh(deformed);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(fine);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_shear_and_reflection(void)
{
    par_shapes_mesh* cage = fold_cage();
    par_shapes_mesh* fine = fold_fine_mesh();
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    // Affine shear pose c' = S c.  The reproduced map is the same shear
    // everywhere, and normals follow the cofactor.
    double S[3][3] = {
        { 1.0, 0.6, 0.0 },
        { 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 1.0 }
    };
    double* shear = PAR_MALLOC(double, 3 * cage->npoints);
    for (int i = 0; i < cage->npoints; i++) {
        double c[3] = { cage->points[3*i+0], cage->points[3*i+1],
            cage->points[3*i+2] };
        mat_mul(S, c, shear + 3 * i);
    }
    par_shapes_mesh* ms = 0;
    TASK_CHECK(par_shapes_cage_apply(b, shear, &ms));
    double cof_shear[3][3] = {
        { 1.0, 0.0, 0.0 },
        { -0.6, 1.0, 0.0 },
        { 0.0, 0.0, 1.0 }
    };
    for (int p = 0; p < FOLD_COUNT; p++) {
        double want[3];
        mat_mul(S, FOLD_POINTS[p], want);
        double got[3] = { ms->points[3*p], ms->points[3*p+1],
            ms->points[3*p+2] };
        TASK_CHECK(vmax3(got, want) <= 2e-5);
        double rn[3] = { fine->normals[3*p+0], fine->normals[3*p+1],
            fine->normals[3*p+2] };
        double en[3];
        mat_mul(cof_shear, rn, en);
        double l = sqrt(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
        for (int k = 0; k < 3; k++) {
            TASK_CHECK(fabs(ms->normals[3 * p + k] - en[k] / l) <= 2e-5);
        }
    }
    par_shapes_free_mesh(ms);

    // Reflection pose: negative determinant must still succeed and preserve
    // the oriented normal via the signed cofactor definition.
    double R[3][3] = {
        { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 },
        { 0.0, 0.0, -1.0 }
    };
    double* reflect = PAR_MALLOC(double, 3 * cage->npoints);
    for (int i = 0; i < cage->npoints; i++) {
        double c[3] = { cage->points[3*i+0], cage->points[3*i+1],
            cage->points[3*i+2] };
        mat_mul(R, c, reflect + 3 * i);
    }
    par_shapes_mesh* mr = 0;
    TASK_CHECK(par_shapes_cage_apply(b, reflect, &mr));
    for (int p = 0; p < FOLD_COUNT; p++) {
        double want[3];
        mat_mul(R, FOLD_POINTS[p], want);
        double got[3] = { mr->points[3*p], mr->points[3*p+1],
            mr->points[3*p+2] };
        TASK_CHECK(vmax3(got, want) <= 2e-5);
        // cof(R) = det(R) R^{-T} = -R for this orthogonal reflection.
        double rn[3] = { fine->normals[3*p+0], fine->normals[3*p+1],
            fine->normals[3*p+2] };
        double cof_n[3] = { -rn[0], -rn[1], rn[2] };
        double l = sqrt(cof_n[0] * cof_n[0] + cof_n[1] * cof_n[1] +
            cof_n[2] * cof_n[2]);
        for (int k = 0; k < 3; k++) {
            TASK_CHECK(fabs(mr->normals[3 * p + k] - cof_n[k] / l)
                <= 2e-5);
        }
    }

    PAR_FREE(reflect);
    PAR_FREE(shear);
    par_shapes_free_mesh(mr);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(fine);
    par_shapes_free_mesh(cage);
    return 0;
}

// ---------------------------------------------------------------------------
// binding group
// ---------------------------------------------------------------------------

static int case_binding_independence_and_reuse(void)
{
    par_shapes_mesh* cage = fold_cage();
    par_shapes_mesh* fine = fold_fine_mesh();
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    int n = cage->npoints;
    double* saved_w = PAR_MALLOC(double, n);
    double* saved_g = PAR_MALLOC(double, 3 * n);
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, saved_w, saved_g));

    // Mutate and free the caller's meshes; the binding must be unaffected.
    cage->points[0] += 5.0f;
    fine->points[1] -= 7.0f;
    double* check_w = PAR_MALLOC(double, n);
    double* check_g = PAR_MALLOC(double, 3 * n);
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, check_w, check_g));
    for (int i = 0; i < n; i++) {
        TASK_CHECK(check_w[i] == saved_w[i]);
        for (int k = 0; k < 3; k++) {
            TASK_CHECK(check_g[3 * i + k] == saved_g[3 * i + k]);
        }
    }
    par_shapes_free_mesh(cage);
    par_shapes_free_mesh(fine);
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, check_w, check_g));
    for (int i = 0; i < n; i++) TASK_CHECK(check_w[i] == saved_w[i]);

    // Repeated applications produce independent meshes with no shared buffer.
    par_shapes_mesh* cage2 = fold_cage();
    double* pose1 = PAR_MALLOC(double, 3 * n);
    double* pose2 = PAR_MALLOC(double, 3 * n);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            pose1[3 * i + k] = cage2->points[3 * i + k];
            pose2[3 * i + k] = cage2->points[3 * i + k];
        }
    }
    for (int k = 0; k < 3; k++) pose2[3 * 1 + k] += 0.2;
    par_shapes_mesh* o1 = 0;
    par_shapes_mesh* o2 = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose1, &o1));
    TASK_CHECK(par_shapes_cage_apply(b, pose2, &o2));
    TASK_CHECK(o1 != o2 && o1->points != o2->points);
    float snapshot = o2->points[0];
    o1->points[0] = 12345.0f;
    TASK_CHECK(o2->points[0] == snapshot);
    par_shapes_free_mesh(o1);
    par_shapes_free_mesh(o2);

    // A failed apply must leave *out and its mesh contents untouched.
    par_shapes_mesh* sentinel = fold_fine_mesh();
    par_shapes_mesh* out = sentinel;
    float sentinel_xy = sentinel->points[4];
    double* bad_pose = PAR_MALLOC(double, 3 * n);
    for (int i = 0; i < 3 * n; i++) bad_pose[i] = (double) i;
    bad_pose[7] = (double) NAN;
    TASK_CHECK(!par_shapes_cage_apply(b, bad_pose, &out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(out->points[4] == sentinel_xy);

    // Collapsing every cage vertex makes every local Jacobian singular.
    double* collapsed = PAR_MALLOC(double, 3 * n);
    for (int i = 0; i < 3 * n; i++) collapsed[i] = 0.1;
    TASK_CHECK(!par_shapes_cage_apply(b, collapsed, &out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(out->points[4] == sentinel_xy);

    PAR_FREE(collapsed);
    PAR_FREE(bad_pose);
    PAR_FREE(pose1);
    PAR_FREE(pose2);
    PAR_FREE(saved_w);
    PAR_FREE(saved_g);
    PAR_FREE(check_w);
    PAR_FREE(check_g);
    par_shapes_free_mesh(sentinel);
    par_shapes_free_mesh(cage2);
    par_shapes_cage_free(b);
    return 0;
}

static int case_apply_preserves_mesh_identity(void)
{
    par_shapes_mesh* cage = fold_cage();

    // Fine mesh with a texture seam: two coincident points carrying distinct
    // normals and distinct UVs, referenced by separate triangles.
    par_shapes_mesh* fine = PAR_CALLOC(par_shapes_mesh, 1);
    fine->npoints = 4;
    fine->ntriangles = 2;
    fine->points = PAR_MALLOC(float, 12);
    fine->normals = PAR_MALLOC(float, 12);
    fine->tcoords = PAR_MALLOC(float, 8);
    fine->triangles = PAR_MALLOC(PAR_SHAPES_T, 6);
    const double seam[3] = { 0.05, 0.05, 0.05 };
    const double other1[3] = { 0.0, 0.0, 0.0 };
    const double other2[3] = { 0.2, 0.1, -0.3 };
    float pts[12] = {
        (float) seam[0], (float) seam[1], (float) seam[2],
        (float) seam[0], (float) seam[1], (float) seam[2],
        (float) other1[0], (float) other1[1], (float) other1[2],
        (float) other2[0], (float) other2[1], (float) other2[2]
    };
    memcpy(fine->points, pts, sizeof(pts));
    float nrm[12] = {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        1, 1, 0
    };
    for (int i = 0; i < 4; i++) {
        double l = sqrt(nrm[3*i]*nrm[3*i] + nrm[3*i+1]*nrm[3*i+1] +
            nrm[3*i+2]*nrm[3*i+2]);
        for (int k = 0; k < 3; k++) nrm[3 * i + k] = (float)(nrm[3*i+k] / l);
    }
    memcpy(fine->normals, nrm, sizeof(nrm));
    float uvs[8] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.25f, 0.75f, 0.5f, 0.5f };
    memcpy(fine->tcoords, uvs, sizeof(uvs));
    fine->triangles[0] = 0;
    fine->triangles[1] = 2;
    fine->triangles[2] = 3;
    fine->triangles[3] = 1;
    fine->triangles[4] = 2;
    fine->triangles[5] = 3;

    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, fine);
    TASK_CHECK(b != 0);

    double* pose = PAR_MALLOC(double, 3 * cage->npoints);
    for (int i = 0; i < cage->npoints; i++) {
        for (int k = 0; k < 3; k++) {
            pose[3 * i + k] = cage->points[3 * i + k];
        }
    }
    pose[5] += 0.4; // nonlinear deformation
    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));

    // Indices and UVs survive verbatim.
    TASK_CHECK(out->ntriangles == fine->ntriangles);
    for (int i = 0; i < 6; i++) {
        TASK_CHECK(out->triangles[i] == fine->triangles[i]);
    }
    TASK_CHECK(out->tcoords != 0);
    for (int i = 0; i < 8; i++) TASK_CHECK(out->tcoords[i] == uvs[i]);

    // Seam copies move together in position but keep their own normals.
    for (int k = 0; k < 3; k++) {
        TASK_CHECK(fabs(out->points[k] - out->points[3 + k]) <= 2e-5);
    }
    double ndiff = 0.0;
    for (int k = 0; k < 3; k++) {
        ndiff += fabs(out->normals[k] - out->normals[3 + k]);
    }
    TASK_CHECK(ndiff > 0.5);
    for (int i = 0; i < 4; i++) {
        double l = sqrt(out->normals[3*i]*out->normals[3*i] +
            out->normals[3*i+1]*out->normals[3*i+1] +
            out->normals[3*i+2]*out->normals[3*i+2]);
        TASK_CHECK(fabs(l - 1.0) <= 2e-5);
    }

    // Rest pose reproduces the original float positions.
    par_shapes_mesh* rest = 0;
    double* rest_pose = PAR_MALLOC(double, 3 * cage->npoints);
    for (int i = 0; i < cage->npoints; i++) {
        for (int k = 0; k < 3; k++) {
            rest_pose[3 * i + k] = cage->points[3 * i + k];
        }
    }
    TASK_CHECK(par_shapes_cage_apply(b, rest_pose, &rest));
    for (int i = 0; i < 12; i++) {
        TASK_CHECK(fabs(rest->points[i] - fine->points[i]) <= 2e-5);
    }

    // A fine mesh without normals keeps null output normals.
    par_shapes_mesh* nonormal =
        point_mesh(FOLD_POINTS[FOLD_FD_A][0], FOLD_POINTS[FOLD_FD_A][1],
            FOLD_POINTS[FOLD_FD_A][2]);
    par_shapes_cage_binding* b2 = par_shapes_cage_bind(cage, nonormal);
    TASK_CHECK(b2 != 0);
    par_shapes_mesh* out2 = 0;
    TASK_CHECK(par_shapes_cage_apply(b2, pose, &out2));
    TASK_CHECK(out2->normals == 0);

    PAR_FREE(rest_pose);
    PAR_FREE(pose);
    par_shapes_free_mesh(out2);
    par_shapes_cage_free(b2);
    par_shapes_free_mesh(nonormal);
    par_shapes_free_mesh(rest);
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(fine);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_binding_failures(void)
{
    par_shapes_mesh* cage = fold_cage();

    // A fine point outside the closed cage cannot be bound.
    par_shapes_mesh* outside = point_mesh(5.0, 5.0, 5.0);
    TASK_CHECK(par_shapes_cage_bind(cage, outside) == 0);
    par_shapes_free_mesh(outside);

    // An open cage (one face removed) is not a closed manifold.
    par_shapes_mesh* open = par_shapes_clone(cage, 0);
    open->ntriangles -= 1;
    par_shapes_mesh* inside = point_mesh(FOLD_POINTS[FOLD_FD_A][0],
        FOLD_POINTS[FOLD_FD_A][1], FOLD_POINTS[FOLD_FD_A][2]);
    TASK_CHECK(par_shapes_cage_bind(open, inside) == 0);

    // A degenerate face is rejected.
    par_shapes_mesh* degenerate = par_shapes_clone(cage, 0);
    degenerate->triangles[1] = degenerate->triangles[0];
    TASK_CHECK(par_shapes_cage_bind(degenerate, inside) == 0);

    // Null arguments fail cleanly.
    TASK_CHECK(par_shapes_cage_bind(0, inside) == 0);
    TASK_CHECK(par_shapes_cage_bind(cage, 0) == 0);
    par_shapes_cage_free(0);

    par_shapes_free_mesh(degenerate);
    par_shapes_free_mesh(open);
    par_shapes_free_mesh(inside);
    par_shapes_free_mesh(cage);
    return 0;
}

const struct task_case task_cases[] = {
    { "coordinates", "tetrahedron", case_tetra_coordinates },
    { "coordinates", "nonconvex_signed", case_nonconvex_signed_coordinates },
    { "differential", "gradient_central_differences",
        case_gradient_central_differences },
    { "differential", "nonaffine_jacobian", case_nonaffine_jacobian },
    { "differential", "shear_reflection_normals", case_shear_and_reflection },
    { "binding", "independence_reuse", case_binding_independence_and_reuse },
    { "binding", "attribute_preservation",
        case_apply_preserves_mesh_identity },
    { "binding", "failure_modes", case_binding_failures }
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
