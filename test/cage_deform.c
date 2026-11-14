// Cage deformation tests.  Registered through task_support/check.h in three
// groups: coordinates, differential and binding.  Central differences appear
// only in this test file and are never used by the library implementation.

#include "par_shapes.h"
#include "../task_support/check.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Small test helpers
// ---------------------------------------------------------------------------

static int nearly(double a, double b, double tol)
{
    double d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

static par_shapes_mesh* make_mesh(int np, const double* xyz,
    const float* nrm, const float* uv, int nt, const PAR_SHAPES_T* tris)
{
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = np;
    m->ntriangles = nt;
    m->points = PAR_MALLOC(float, 3 * np);
    for (int i = 0; i < 3 * np; i++) m->points[i] = (float) xyz[i];
    if (nt > 0) {
        m->triangles = PAR_MALLOC(PAR_SHAPES_T, 3 * nt);
        memcpy(m->triangles, tris, sizeof(PAR_SHAPES_T) * 3 * nt);
    }
    if (nrm) {
        m->normals = PAR_MALLOC(float, 3 * np);
        memcpy(m->normals, nrm, sizeof(float) * 3 * np);
    }
    if (uv) {
        m->tcoords = PAR_MALLOC(float, 2 * np);
        memcpy(m->tcoords, uv, sizeof(float) * 2 * np);
    }
    return m;
}

// Non-convex extruded L prism: 12 ring vertices plus two fan centers.
static par_shapes_mesh* make_l_cage(void)
{
    static const double poly[6][2] = {
        {0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}
    };
    const int m = 6;
    double xyz[14 * 3];
    for (int i = 0; i < m; i++) {
        xyz[3 * i + 0] = poly[i][0];
        xyz[3 * i + 1] = poly[i][1];
        xyz[3 * i + 2] = -1.0;
        xyz[3 * (m + i) + 0] = poly[i][0];
        xyz[3 * (m + i) + 1] = poly[i][1];
        xyz[3 * (m + i) + 2] = 1.0;
    }
    xyz[3 * 12 + 0] = 0.5; xyz[3 * 12 + 1] = 0.5; xyz[3 * 12 + 2] = -1.0;
    xyz[3 * 13 + 0] = 0.5; xyz[3 * 13 + 1] = 0.5; xyz[3 * 13 + 2] = 1.0;
    PAR_SHAPES_T tris[24 * 3];
    int n = 0;
    for (int a = 0; a < m; a++) {
        int b = (a + 1) % m;
        tris[n++] = 12; tris[n++] = (PAR_SHAPES_T) b; tris[n++] = (PAR_SHAPES_T) a;
        tris[n++] = 13; tris[n++] = (PAR_SHAPES_T) (m + a);
        tris[n++] = (PAR_SHAPES_T) (m + b);
        tris[n++] = (PAR_SHAPES_T) a; tris[n++] = (PAR_SHAPES_T) b;
        tris[n++] = (PAR_SHAPES_T) (m + b);
        tris[n++] = (PAR_SHAPES_T) a; tris[n++] = (PAR_SHAPES_T) (m + b);
        tris[n++] = (PAR_SHAPES_T) (m + a);
    }
    return make_mesh(14, xyz, 0, 0, 24, tris);
}

// Verify the four MVC identities for one point.  Returns 0 on success.
static int check_identities(const par_shapes_cage_binding* b,
    const par_shapes_mesh* cage, int point, const double* x)
{
    int n = cage->npoints;
    double* w = PAR_MALLOC(double, n);
    double* g = PAR_MALLOC(double, 3 * n);
    TASK_CHECK(par_shapes_cage_coordinates(b, point, w, g));

    double sumw = 0;
    for (int i = 0; i < n; i++) sumw += w[i];
    TASK_CHECK(nearly(sumw, 1.0, 1e-7));

    for (int k = 0; k < 3; k++) {
        double s = 0;
        for (int i = 0; i < n; i++) s += g[3 * i + k];
        TASK_CHECK(nearly(s, 0.0, 1e-7));
    }

    for (int k = 0; k < 3; k++) {
        double repro = 0;
        for (int i = 0; i < n; i++)
            repro += w[i] * (double) cage->points[3 * i + k];
        TASK_CHECK(nearly(repro, x[k], 1e-7));
    }

    for (int a = 0; a < 3; a++) {
        for (int c = 0; c < 3; c++) {
            double v = 0;
            for (int i = 0; i < n; i++)
                v += (double) cage->points[3 * i + a] * g[3 * i + c];
            TASK_CHECK(nearly(v, a == c ? 1.0 : 0.0, 1e-7));
        }
    }
    PAR_FREE(w);
    PAR_FREE(g);
    return 0;
}

static double vec3_dot(const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static double mat3_det(const double* J)
{
    return J[0] * (J[4] * J[8] - J[5] * J[7])
        - J[1] * (J[3] * J[8] - J[5] * J[6])
        + J[2] * (J[3] * J[7] - J[4] * J[6]);
}

// Cofactor matrix det(J) * inverse(J)^T via 2x2 minors, row-major.
static void mat3_cofactor(const double* J, double* cof)
{
    cof[0] = J[4] * J[8] - J[5] * J[7];
    cof[1] = J[5] * J[6] - J[3] * J[8];
    cof[2] = J[3] * J[7] - J[4] * J[6];
    cof[3] = J[2] * J[7] - J[1] * J[8];
    cof[4] = J[0] * J[8] - J[2] * J[6];
    cof[5] = J[1] * J[6] - J[0] * J[7];
    cof[6] = J[1] * J[5] - J[2] * J[4];
    cof[7] = J[2] * J[3] - J[0] * J[5];
    cof[8] = J[0] * J[4] - J[1] * J[3];
}

// Test-side analytic Jacobian from the public gradients and a cage pose.
static int test_jacobian(const par_shapes_cage_binding* b, int ncage,
    int point, const double* pose, double* J)
{
    double* w = PAR_MALLOC(double, ncage);
    double* g = PAR_MALLOC(double, 3 * ncage);
    TASK_CHECK(par_shapes_cage_coordinates(b, point, w, g));
    memset(J, 0, sizeof(double) * 9);
    for (int i = 0; i < ncage; i++)
        for (int a = 0; a < 3; a++)
            for (int c = 0; c < 3; c++)
                J[3 * a + c] += pose[3 * i + a] * g[3 * i + c];
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
    // The exact centroid (the generator's tetra is slightly asymmetric).
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (int i = 0; i < 4; i++) {
        cx += cage->points[3 * i + 0];
        cy += cage->points[3 * i + 1];
        cz += cage->points[3 * i + 2];
    }
    cx /= 4.0; cy /= 4.0; cz /= 4.0;
    double fine[5 * 3] = {
        cx, cy, cz,
        cx + 0.05, cy, cz,
        cx - 0.05, cy + 0.05, cz,
        cx, cy - 0.05, cz + 0.05,
        cx, cy, cz - 0.08
    };
    par_shapes_mesh* mesh = make_mesh(5, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b != 0);

    for (int p = 0; p < 5; p++)
        TASK_CHECK(check_identities(b, cage, p, fine + 3 * p) == 0);

    // At the centroid of a tetrahedron the four weights are equal.
    double w[4], g[12];
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w, g));
    for (int i = 0; i < 4; i++) TASK_CHECK(nearly(w[i], 0.25, 1e-7));

    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_nonconvex_coordinates(void)
{
    par_shapes_mesh* cage = make_l_cage();
    double fine[3 * 3] = {
        0.5, 0.5, 0.0,
        1.5, 0.5, 0.3,
        0.5, 1.5, -0.4
    };
    par_shapes_mesh* mesh = make_mesh(3, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b != 0);
    for (int p = 0; p < 3; p++)
        TASK_CHECK(check_identities(b, cage, p, fine + 3 * p) == 0);

    // Signed MVC: points near the reentrant corner must show negative weights.
    int saw_negative = 0;
    for (int p = 0; p < 3; p++) {
        double w[14], g[42];
        TASK_CHECK(par_shapes_cage_coordinates(b, p, w, g));
        double sumw = 0;
        for (int i = 0; i < 14; i++) {
            sumw += w[i];
            if (w[i] < 0) saw_negative = 1;
        }
        TASK_CHECK(nearly(sumw, 1.0, 1e-7));
    }
    TASK_CHECK(saw_negative);

    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_winding_invariance(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    par_shapes_mesh* reversed = par_shapes_clone(cage, 0);
    par_shapes_invert(reversed, 0, 0);
    double fine[2 * 3] = {0.0, 1.333 / 4.0, 0.0, 0.07, 1.333 / 4.0, -0.04};
    par_shapes_mesh* mesh = make_mesh(2, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b1 = par_shapes_cage_bind(cage, mesh);
    par_shapes_cage_binding* b2 = par_shapes_cage_bind(reversed, mesh);
    TASK_CHECK(b1 && b2);
    for (int p = 0; p < 2; p++) {
        double w1[4], g1[12], w2[4], g2[12];
        TASK_CHECK(par_shapes_cage_coordinates(b1, p, w1, g1));
        TASK_CHECK(par_shapes_cage_coordinates(b2, p, w2, g2));
        for (int i = 0; i < 4; i++) {
            TASK_CHECK(nearly(w1[i], w2[i], 1e-10));
            for (int k = 0; k < 3; k++)
                TASK_CHECK(nearly(g1[3 * i + k], g2[3 * i + k], 1e-9));
        }
    }
    par_shapes_cage_free(b1);
    par_shapes_cage_free(b2);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(reversed);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_uniform_scale(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double fine[1 * 3] = {0.07, 1.333 / 4.0, -0.04};
    par_shapes_mesh* mesh = make_mesh(1, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b1 = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b1);

    // Powers of two scale float geometry exactly (exponent-only), so the two
    // cages are a true uniform scaling even after float storage.
    const double factors[2] = {8.0, 0.5};
    for (int q = 0; q < 2; q++) {
        double s = factors[q];
        double scxyz[12], smxyz[3];
        for (int i = 0; i < 12; i++) scxyz[i] = (double) cage->points[i] * s;
        for (int i = 0; i < 3; i++) smxyz[i] = fine[i] * s;
        par_shapes_mesh* sc = make_mesh(4, scxyz, 0, 0,
            cage->ntriangles, cage->triangles);
        par_shapes_mesh* sm = make_mesh(1, smxyz, 0, 0, 0, 0);
        par_shapes_cage_binding* b2 = par_shapes_cage_bind(sc, sm);
        TASK_CHECK(b2);
        double w1[4], g1[12], w2[4], g2[12];
        TASK_CHECK(par_shapes_cage_coordinates(b1, 0, w1, g1));
        TASK_CHECK(par_shapes_cage_coordinates(b2, 0, w2, g2));
        for (int i = 0; i < 4; i++) {
            TASK_CHECK(nearly(w1[i], w2[i], 1e-9));
            for (int k = 0; k < 3; k++)
                TASK_CHECK(nearly(g2[3 * i + k], g1[3 * i + k] / s, 1e-8));
        }
        par_shapes_cage_free(b2);
        par_shapes_free_mesh(sm);
        par_shapes_free_mesh(sc);
    }
    par_shapes_cage_free(b1);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

// ---------------------------------------------------------------------------
// differential group
// ---------------------------------------------------------------------------

static int case_jacobian_finite_difference(void)
{
    // A tetrahedron cage gives affine (barycentric) coordinates, so a genuinely
    // nonlinear field needs the 14-vertex non-convex L prism.
    par_shapes_mesh* cage = make_l_cage();
    const int nc = cage->npoints;

    // h = 2^-10 is exactly representable; the stencil sits on exact floats
    // around (0.5, 0.5, 0), well inside the L and away from its faces.
    const double h = 1.0 / 1024.0;
    // Order: center, +/- each axis (FD stencil), then an offset second point.
    double fine[8 * 3] = {
        0.5, 0.5, 0.0,
        0.5 + h, 0.5, 0.0, 0.5 - h, 0.5, 0.0,
        0.5, 0.5 + h, 0.0, 0.5, 0.5 - h, 0.0,
        0.5, 0.5, h, 0.5, 0.5, -h,
        1.5, 0.5, 0.3
    };
    par_shapes_mesh* mesh = make_mesh(8, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    // Clearly non-affine pose: drag one ring cage vertex outward.
    double* pose = PAR_MALLOC(double, 3 * nc);
    for (int i = 0; i < nc; i++)
        for (int k = 0; k < 3; k++)
            pose[3 * i + k] = (double) cage->points[3 * i + k];
    pose[3 * 2 + 0] += 0.4;
    pose[3 * 2 + 2] += 0.3;

    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    TASK_CHECK(out && out->npoints == 8);

    double Jan[9];
    TASK_CHECK(test_jacobian(b, nc, 0, pose, Jan) == 0);

    // Independent central differences of the deformed map, evaluated entirely
    // in double from the public weights at the stencil points (the cached
    // gradients are deliberately not used on the reference side).
    for (int k = 0; k < 3; k++) {
        double* wp = PAR_MALLOC(double, nc);
        double* wm = PAR_MALLOC(double, nc);
        double* dummy = PAR_MALLOC(double, 3 * nc);
        int ip = 1 + 2 * k, im = 2 + 2 * k;
        TASK_CHECK(par_shapes_cage_coordinates(b, ip, wp, dummy));
        TASK_CHECK(par_shapes_cage_coordinates(b, im, wm, dummy));
        for (int a = 0; a < 3; a++) {
            double yp = 0, ym = 0;
            for (int i = 0; i < nc; i++) {
                yp += wp[i] * pose[3 * i + a];
                ym += wm[i] * pose[3 * i + a];
            }
            double fd = (yp - ym) / (2 * h);
            TASK_CHECK(nearly(Jan[3 * a + k], fd, 1e-5));
        }
        PAR_FREE(wp);
        PAR_FREE(wm);
        PAR_FREE(dummy);
    }

    // The Jacobian must vary between rest points under a non-affine pose.
    double Jother[9];
    TASK_CHECK(test_jacobian(b, nc, 7, pose, Jother) == 0);
    double maxdiff = 0;
    for (int i = 0; i < 9; i++) {
        double d = Jan[i] - Jother[i];
        if (d < 0) d = -d;
        if (d > maxdiff) maxdiff = d;
    }
    TASK_CHECK(maxdiff > 1e-4);

    PAR_FREE(pose);
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_shear(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double x[3] = {0.07, 1.333 / 4.0, -0.04};
    par_shapes_mesh* mesh = make_mesh(1, x, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    // Affine shear A: (x, y, z) -> (x + 0.5 z, y, z).
    double A[9] = {1, 0, 0.5, 0, 1, 0, 0, 0, 1};
    double pose[12];
    for (int i = 0; i < 4; i++)
        for (int r = 0; r < 3; r++) {
            double v = 0;
            for (int c = 0; c < 3; c++)
                v += A[3 * r + c] * (double) cage->points[3 * i + c];
            pose[3 * i + r] = v;
        }
    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    for (int r = 0; r < 3; r++) {
        double expected = 0;
        for (int c = 0; c < 3; c++) expected += A[3 * r + c] * x[c];
        TASK_CHECK(nearly((double) out->points[r], expected, 2e-5));
    }
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_reflection_normals(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double x[2 * 3] = {
        0.0, 1.333 / 4.0, 0.0,
        0.06, 1.333 / 4.0 + 0.05, -0.05
    };
    float nrm[2 * 3] = {
        0.36f, 0.48f, 0.8f,       // unit length
        0.6f, -0.8f, 0.0f
    };
    par_shapes_mesh* mesh = make_mesh(2, x, nrm, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    // Reflection across the yz plane: x component flips, det(A) = -1.
    double A[9] = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
    double pose[12];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            pose[3 * i + k] = A[3 * k + k] *
                (double) cage->points[3 * i + k];

    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    TASK_CHECK(out->normals != 0);

    double cof[9];
    mat3_cofactor(A, cof);
    for (int p = 0; p < 2; p++) {
        double expected[3] = {0, 0, 0};
        // Normals use C^T = det(A) inverse(A)^T.
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                expected[r] += cof[3 * c + r] * (double) nrm[3 * p + c];
        double len = sqrt(vec3_dot(expected, expected));
        for (int r = 0; r < 3; r++)
            TASK_CHECK(nearly((double) out->normals[3 * p + r],
                expected[r] / len, 2e-5));
        // Reflected normal keeps its oriented sign: the x component is
        // unchanged because cof(diag(-1,1,1))^T = diag(1,-1,-1).
        TASK_CHECK(out->normals[3 * p + 0] * nrm[3 * p + 0] > 0);
    }
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_nonlinear_normals(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double x[2 * 3] = {
        0.0, 1.333 / 4.0, 0.0,
        -0.06, 1.333 / 4.0 - 0.04, 0.07
    };
    float nrm[2 * 3] = {
        0.0f, 0.6f, 0.8f,
        1.0f, 0.0f, 0.0f
    };
    par_shapes_mesh* mesh = make_mesh(2, x, nrm, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    double pose[12];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            pose[3 * i + k] = (double) cage->points[3 * i + k];
    pose[3 * 2 + 0] += 0.4;   // asymmetric vertex move -> nonlinear field
    pose[3 * 1 + 2] -= 0.25;

    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    TASK_CHECK(out->normals != 0);
    for (int p = 0; p < 2; p++) {
        double J[9], cof[9];
        TASK_CHECK(test_jacobian(b, 4, p, pose, J) == 0);
        TASK_CHECK(mat3_det(J) != 0.0);
        // A non-symmetric Jacobian makes the C^T normal rule distinguishable
        // from applying C directly, so this case actually guards the transpose.
        double asym = 0.0;
        for (int r = 0; r < 3; r++)
            for (int c = r + 1; c < 3; c++) {
                double d = J[3 * r + c] - J[3 * c + r];
                if (d < 0) d = -d;
                if (d > asym) asym = d;
            }
        TASK_CHECK(asym > 1e-4);
        mat3_cofactor(J, cof);
        double expected[3] = {0, 0, 0};
        // Normals use C^T = det(J) inverse(J)^T.
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                expected[r] += cof[3 * c + r] * (double) nrm[3 * p + c];
        double len = sqrt(vec3_dot(expected, expected));
        TASK_CHECK(len > 0.0);
        for (int r = 0; r < 3; r++)
            TASK_CHECK(nearly((double) out->normals[3 * p + r],
                expected[r] / len, 2e-5));
        double unit = (double) out->normals[3 * p] * out->normals[3 * p]
            + (double) out->normals[3 * p + 1] * out->normals[3 * p + 1]
            + (double) out->normals[3 * p + 2] * out->normals[3 * p + 2];
        TASK_CHECK(nearly(unit, 1.0, 2e-5));
    }
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_uv_seam(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double px = 0.0, py = 1.333 / 4.0, pz = 0.0;
    // Two seam copies share a position but carry distinct normals and UVs.
    double xyz[3 * 3] = {
        px, py, pz,
        px, py, pz,
        px + 0.05, py - 0.05, pz
    };
    float nrm[3 * 3] = {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1
    };
    float uv[3 * 2] = {0, 0, 1, 1, 0.5f, 0.5f};
    PAR_SHAPES_T tris[3] = {0, 1, 2};
    par_shapes_mesh* mesh = make_mesh(3, xyz, nrm, uv, 1, tris);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    double pose[12];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            pose[3 * i + k] = (double) cage->points[3 * i + k];
    pose[3 * 3 + 1] += 0.5;

    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    // Seam copies are not welded: identical positions, distinct normals.
    for (int k = 0; k < 3; k++)
        TASK_CHECK(out->points[3 * 0 + k] == out->points[3 * 1 + k]);
    int normals_differ = 0;
    for (int k = 0; k < 3; k++)
        if (out->normals[3 * 0 + k] != out->normals[3 * 1 + k])
            normals_differ = 1;
    TASK_CHECK(normals_differ);
    // UVs and indices survive bit-for-bit.
    TASK_CHECK(memcmp(out->tcoords, uv, sizeof(uv)) == 0);
    TASK_CHECK(out->ntriangles == 1 && out->triangles[0] == 0 &&
        out->triangles[1] == 1 && out->triangles[2] == 2);
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

// ---------------------------------------------------------------------------
// binding group
// ---------------------------------------------------------------------------

static int case_source_independence(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double fine[2 * 3] = {0.0, 1.333 / 4.0, 0.0, 0.05, 1.333 / 4.0, 0.05};
    par_shapes_mesh* mesh = make_mesh(2, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    double w0[4], g0[12];
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w0, g0));

    // Mutate and then destroy the caller-owned sources.
    par_shapes_translate(cage, 7.0f, -3.0f, 2.0f);
    par_shapes_translate(mesh, -9.0f, 4.0f, 1.0f);
    double w1[4], g1[12];
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w1, g1));
    for (int i = 0; i < 4; i++) {
        TASK_CHECK(nearly(w0[i], w1[i], 1e-12));
        for (int k = 0; k < 3; k++)
            TASK_CHECK(nearly(g0[3 * i + k], g1[3 * i + k], 1e-12));
    }
    par_shapes_free_mesh(cage);
    par_shapes_free_mesh(mesh);
    double w2[4], g2[12];
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w2, g2));
    for (int i = 0; i < 4; i++) TASK_CHECK(nearly(w0[i], w2[i], 1e-12));

    par_shapes_cage_free(b);
    par_shapes_cage_free(0);  // must be a no-op
    return 0;
}

static int case_repeated_apply_ownership(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double fine[2 * 3] = {0.0, 1.333 / 4.0, 0.0, 0.05, 1.333 / 4.0, 0.05};
    par_shapes_mesh* mesh = make_mesh(2, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    double rest[12], pose[12];
    for (int i = 0; i < 12; i++) rest[i] = (double) cage->points[i];
    par_shapes_mesh* identity_out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, rest, &identity_out));
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++)
            TASK_CHECK(nearly((double) identity_out->points[3 * p + k],
                fine[3 * p + k], 2e-5));

    memcpy(pose, rest, sizeof(pose));
    pose[3 * 0 + 1] += 0.5;
    par_shapes_mesh* out1 = 0;
    par_shapes_mesh* out2 = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out1));
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out2));
    TASK_CHECK(out1 != out2 && out1->points != out2->points);
    for (int i = 0; i < 6; i++)
        TASK_CHECK(nearly((double) out1->points[i],
            (double) out2->points[i], 1e-12));

    // A later pose starts from rest data: move a different cage vertex and
    // verify the result independently from the cached weights, while also
    // confirming that vertex 0's earlier y-move is no longer present.
    double pose2[12];
    memcpy(pose2, rest, sizeof(pose2));
    pose2[3 * 1 + 0] += 0.3;
    double w0[4], g0d[12];
    TASK_CHECK(par_shapes_cage_coordinates(b, 0, w0, g0d));
    double expected0[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            expected0[k] += w0[i] * pose2[3 * i + k];
    par_shapes_mesh* out3 = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose2, &out3));
    for (int k = 0; k < 3; k++)
        TASK_CHECK(nearly((double) out3->points[k], expected0[k], 2e-5));
    // pose2 only changes a cage vertex's x, so the first fine point's y is
    // back at its identity value: the earlier pose's y-move did not persist.
    TASK_CHECK(nearly((double) out3->points[1],
        (double) identity_out->points[1], 2e-5));

    par_shapes_free_mesh(identity_out);
    par_shapes_free_mesh(out1);
    par_shapes_free_mesh(out2);
    par_shapes_free_mesh(out3);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_failure_paths(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double fine[1 * 3] = {0.0, 1.333 / 4.0, 0.0};
    par_shapes_mesh* mesh = make_mesh(1, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, mesh);
    TASK_CHECK(b);

    TASK_CHECK(par_shapes_cage_bind(0, mesh) == 0);
    TASK_CHECK(par_shapes_cage_bind(cage, 0) == 0);
    double w[4], g[12];
    TASK_CHECK(!par_shapes_cage_coordinates(b, -1, w, g));
    TASK_CHECK(!par_shapes_cage_coordinates(b, 99, w, g));
    TASK_CHECK(!par_shapes_cage_coordinates(0, 0, w, g));
    par_shapes_mesh* out = 0;
    TASK_CHECK(!par_shapes_cage_apply(0, 0, &out));

    // Failure must preserve the previous *out value and its mesh contents.
    par_shapes_mesh* sentinel = par_shapes_create_tetrahedron();
    int saved_npoints = sentinel->npoints;
    float saved_first = sentinel->points[0];
    out = sentinel;
    double bad_pose[12];
    for (int i = 0; i < 12; i++) bad_pose[i] = (double) cage->points[i];
    bad_pose[3] = NAN;
    TASK_CHECK(!par_shapes_cage_apply(b, bad_pose, &out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(out->npoints == saved_npoints);
    TASK_CHECK(out->points[0] == saved_first);

    // Collapsed cage pose gives a singular Jacobian.
    double collapsed[12];
    for (int i = 0; i < 12; i++) collapsed[i] = 0.0;
    TASK_CHECK(!par_shapes_cage_apply(b, collapsed, &out));
    TASK_CHECK(out == sentinel);
    par_shapes_free_mesh(sentinel);

    // A fine point outside the cage cannot form finite interior coordinates.
    double outside[1 * 3] = {10.0, 10.0, 10.0};
    par_shapes_mesh* outer = make_mesh(1, outside, 0, 0, 0, 0);
    TASK_CHECK(par_shapes_cage_bind(cage, outer) == 0);
    par_shapes_free_mesh(outer);

    // An open cage (one face removed) is not a closed 2-manifold.
    par_shapes_mesh* open_cage = par_shapes_clone(cage, 0);
    open_cage->ntriangles = 3;
    TASK_CHECK(par_shapes_cage_bind(open_cage, mesh) == 0);
    par_shapes_free_mesh(open_cage);

    // A degenerate face is rejected.
    par_shapes_mesh* deg = par_shapes_clone(cage, 0);
    deg->triangles[2] = deg->triangles[0];
    TASK_CHECK(par_shapes_cage_bind(deg, mesh) == 0);
    par_shapes_free_mesh(deg);

    par_shapes_cage_free(b);
    par_shapes_free_mesh(mesh);
    par_shapes_free_mesh(cage);
    return 0;
}

static int case_optional_attributes(void)
{
    par_shapes_mesh* cage = par_shapes_create_tetrahedron();
    double fine[1 * 3] = {0.0, 1.333 / 4.0, 0.0};
    par_shapes_mesh* plain = make_mesh(1, fine, 0, 0, 0, 0);
    par_shapes_cage_binding* b = par_shapes_cage_bind(cage, plain);
    TASK_CHECK(b);
    double pose[12];
    for (int i = 0; i < 12; i++) pose[i] = (double) cage->points[i];
    pose[1] += 0.4;
    par_shapes_mesh* out = 0;
    TASK_CHECK(par_shapes_cage_apply(b, pose, &out));
    TASK_CHECK(out->normals == 0);
    TASK_CHECK(out->tcoords == 0);
    par_shapes_free_mesh(out);
    par_shapes_cage_free(b);
    par_shapes_free_mesh(plain);
    par_shapes_free_mesh(cage);
    return 0;
}

const struct task_case task_cases[] = {
    {"coordinates", "tetra", case_tetra_coordinates},
    {"coordinates", "nonconvex", case_nonconvex_coordinates},
    {"coordinates", "winding", case_winding_invariance},
    {"coordinates", "uniform_scale", case_uniform_scale},
    {"differential", "jacobian", case_jacobian_finite_difference},
    {"differential", "shear", case_shear},
    {"differential", "reflection", case_reflection_normals},
    {"differential", "nonlinear_normals", case_nonlinear_normals},
    {"differential", "uv_seam", case_uv_seam},
    {"binding", "source_independence", case_source_independence},
    {"binding", "repeated_apply", case_repeated_apply_ownership},
    {"binding", "failure_paths", case_failure_paths},
    {"binding", "optional_attributes", case_optional_attributes},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
