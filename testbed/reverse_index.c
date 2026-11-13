//---------------------------------------------------------------------------------
//
//  Little Color Management System
//  Copyright (c) 1998-2026 Marti Maria Saguer
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//---------------------------------------------------------------------------------
//
//  C tests for the full-branch reverse CLUT index (cmsReverseCLUT*).
//  The exhaustive reference below derives tetrahedron membership and barycentric
//  coordinates on its own; it never calls cmsReverseCLUTQuery().
//

#include "lcms2.h"
#include "lcms2_internal.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#define REV_TEST_TOL 2e-6

// ------------------------------------------------------------------------------------------------
// Small linear algebra helpers, independent from the library implementation.
// ------------------------------------------------------------------------------------------------

static
double RefDet(const double a[3][3])
{
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
         - a[1][0] * (a[0][1] * a[2][2] - a[0][2] * a[2][1])
         + a[2][0] * (a[0][1] * a[1][2] - a[0][2] * a[1][1]);
}

static
int RefSolve(const double a[3][3], const double b[3], double x[3])
{
    double d = RefDet(a), m[3][3];
    if (fabs(d) < DBL_MIN) return 0;

    memcpy(m, a, sizeof(m));
    m[0][0] = b[0]; m[1][0] = b[1]; m[2][0] = b[2];
    x[0] = RefDet(m) / d;

    memcpy(m, a, sizeof(m));
    m[0][1] = b[0]; m[1][1] = b[1]; m[2][1] = b[2];
    x[1] = RefDet(m) / d;

    memcpy(m, a, sizeof(m));
    m[0][2] = b[0]; m[1][2] = b[1]; m[2][2] = b[2];
    x[2] = RefDet(m) / d;
    return 1;
}

// ------------------------------------------------------------------------------------------------
// Independent exhaustive reference.
//
// For every grid cell we walk the six regions of the Sakamoto tetrahedral partition, defined
// purely by the ordering of (rx, ry, rz). The four corner nodes of each region are re-derived
// from the corner-value differences used by the forward interpolator. Barycentric coordinates
// come from our own Cramer inversion of the output affine map.
// ------------------------------------------------------------------------------------------------

typedef struct {
    double in[3];
} RefPoint;

static
int RefCompare(const void* a, const void* b)
{
    const RefPoint* pa = (const RefPoint*) a;
    const RefPoint* pb = (const RefPoint*) b;
    int i;
    for (i = 0; i < 3; i++) {
        if (pa->in[i] < pb->in[i]) return -1;
        if (pa->in[i] > pb->in[i]) return 1;
    }
    return 0;
}

// Corner (ix,iy,iz) bits for region r, re-derived independently from the interpolation
// formulas: output = c0 + c1*rx + c2*ry + c3*rz with each difference taken between the
// listed nodes.
static void RefCorners(int region, int bits[4][3])
{
    switch (region) {
        case 0: // c1=100-000, c2=110-100, c3=111-110
            memcpy(bits, (int[4][3]){{0,0,0},{1,0,0},{1,1,0},{1,1,1}}, 4 * sizeof(int[3]));
            break;
        case 1: // c1=100-000, c2=111-101, c3=101-100
            memcpy(bits, (int[4][3]){{0,0,0},{1,0,0},{1,1,1},{1,0,1}}, 4 * sizeof(int[3]));
            break;
        case 2: // c1=101-001, c2=111-101, c3=001-000
            memcpy(bits, (int[4][3]){{0,0,0},{1,0,1},{1,1,1},{0,0,1}}, 4 * sizeof(int[3]));
            break;
        case 3: // c1=110-010, c2=010-000, c3=111-110
            memcpy(bits, (int[4][3]){{0,0,0},{1,1,0},{0,1,0},{1,1,1}}, 4 * sizeof(int[3]));
            break;
        case 4: // c1=111-011, c2=010-000, c3=011-010
            memcpy(bits, (int[4][3]){{0,0,0},{1,1,1},{0,1,0},{0,1,1}}, 4 * sizeof(int[3]));
            break;
        default: // c1=111-011, c2=011-001, c3=001-000
            memcpy(bits, (int[4][3]){{0,0,0},{1,1,1},{0,1,1},{0,0,1}}, 4 * sizeof(int[3]));
            break;
    }
}

static
int RefEnumerate(const cmsUInt32Number dims[3], const cmsFloat32Number* table,
                 const double target[3], RefPoint* out, int maxOut)
{
    int nx = (int) dims[0], ny = (int) dims[1], nz = (int) dims[2];
    int x0, y0, z0, region, n = 0, v, c, i, j;

    for (x0 = 0; x0 < nx - 1; x0++) {
        for (y0 = 0; y0 < ny - 1; y0++) {
            for (z0 = 0; z0 < nz - 1; z0++) {
                for (region = 0; region < 6; region++) {
                        int bits[4][3], node[4];
                        double q[4][3], a[3][3], b[3], w[4], scale = 0.0, det;

                        RefCorners(region, bits);

                        for (v = 0; v < 4; v++) {
                            int ix = x0 + bits[v][0];
                            int iy = y0 + bits[v][1];
                            int iz = z0 + bits[v][2];
                            node[v] = (ix * ny + iy) * nz + iz;
                            for (c = 0; c < 3; c++)
                                q[v][c] = (double) table[node[v] * 3 + c];
                        }

                        for (c = 0; c < 3; c++) {
                            a[c][0] = q[1][c] - q[0][c];
                            a[c][1] = q[2][c] - q[0][c];
                            a[c][2] = q[3][c] - q[0][c];
                            b[c] = target[c] - q[0][c];
                            for (v = 0; v < 3; v++)
                                if (fabs(a[c][v]) > scale) scale = fabs(a[c][v]);
                        }
                        det = RefDet(a);

                        // Singular tetrahedra produce no discrete roots, mirroring the API.
                        if (scale == 0.0 || fabs(det) <= 1e-12 * scale * scale * scale)
                            continue;
                        if (!RefSolve(a, b, &w[1]))
                            continue;
                        w[0] = 1.0 - w[1] - w[2] - w[3];

                        for (v = 0; v < 4; v++)
                            if (w[v] < -REV_TEST_TOL) break;
                        if (v < 4) continue;
                        for (v = 0; v < 4; v++)
                            if (w[v] < 0.0) w[v] = 0.0;

                        if (n >= maxOut) return -1;
                        for (c = 0; c < 3; c++) out[n].in[c] = 0.0;
                        for (v = 0; v < 4; v++) {
                            double corner[3];
                            corner[0] = (double) (x0 + bits[v][0]) / (nx - 1);
                            corner[1] = (double) (y0 + bits[v][1]) / (ny - 1);
                            corner[2] = (double) (z0 + bits[v][2]) / (nz - 1);
                            for (c = 0; c < 3; c++)
                                out[n].in[c] += w[v] * corner[c];
                        }
                        n++;
                    }
            }
        }
    }

    qsort(out, (size_t) n, sizeof(RefPoint), RefCompare);

    // Same tolerance-based deduplication as the public contract.
    j = 0;
    for (i = 0; i < n; i++) {
        int duplicate = 0, k;
        for (k = 0; k < j; k++) {
            double d = 0.0;
            for (c = 0; c < 3; c++) {
                double dd = fabs(out[i].in[c] - out[k].in[c]);
                if (dd > d) d = dd;
            }
            if (d <= REV_TEST_TOL) { duplicate = 1; break; }
        }
        if (!duplicate) {
            if (j != i) out[j] = out[i];
            j++;
        }
    }
    return j;
}

// ------------------------------------------------------------------------------------------------
// Table and pipeline construction helpers.
// ------------------------------------------------------------------------------------------------

static
cmsPipeline* MakePipeline(const cmsUInt32Number dims[3], const cmsFloat32Number* table)
{
    cmsPipeline* lut = cmsPipelineAlloc(NULL, 3, 3);
    cmsStage* stage;
    if (lut == NULL) return NULL;
    stage = cmsStageAllocCLutFloatGranular(NULL, dims, 3, 3, table);
    if (stage == NULL) { cmsPipelineFree(lut); return NULL; }
    if (!cmsPipelineInsertStage(lut, cmsAT_END, stage)) {
        cmsStageFree(stage);
        cmsPipelineFree(lut);
        return NULL;
    }
    return lut;
}

static
void FillTable(const cmsUInt32Number dims[3], cmsFloat32Number* table,
               void (*fn)(int ix, int iy, int iz, double out[3]))
{
    cmsUInt32Number ix, iy, iz;
    for (ix = 0; ix < dims[0]; ix++)
        for (iy = 0; iy < dims[1]; iy++)
            for (iz = 0; iz < dims[2]; iz++) {
                cmsUInt32Number node = (ix * dims[1] + iy) * dims[2] + iz;
                double out[3];
                fn((int) ix, (int) iy, (int) iz, out);
                table[node * 3 + 0] = (cmsFloat32Number) out[0];
                table[node * 3 + 1] = (cmsFloat32Number) out[1];
                table[node * 3 + 2] = (cmsFloat32Number) out[2];
            }
}

static
int SameRootSet(const cmsFloat32Number roots[][3], cmsUInt32Number count, const RefPoint* ref, int refCount)
{
    cmsUInt32Number i;
    int c;
    if ((int) count != refCount) return 0;
    for (i = 0; i < count; i++) {
        double d = 0.0;
        for (c = 0; c < 3; c++) {
            double dd = fabs((double) roots[i][c] - ref[i].in[c]);
            if (dd > d) d = dd;
        }
        if (d > REV_TEST_TOL) return 0;
    }
    return 1;
}

static
int LexSorted(const cmsFloat32Number roots[][3], cmsUInt32Number count)
{
    cmsUInt32Number i;
    int c;
    for (i = 1; i < count; i++) {
        for (c = 0; c < 3; c++) {
            if (roots[i][c] < roots[i - 1][c]) return 0;
            if (roots[i][c] > roots[i - 1][c]) break;
        }
    }
    return 1;
}

static
void CheckAgainstReference(const cmsReverseCLUT* idx, const cmsUInt32Number dims[3],
                           const cmsFloat32Number* table, const cmsFloat32Number target[3],
                           cmsPipeline* lut)
{
    cmsUInt32Number count = 999;
    cmsBool singular = TRUE;
    cmsFloat32Number roots[256][3];
    RefPoint ref[256];
    double tgt[3] = { target[0], target[1], target[2] };
    int refCount, c, i;

    refCount = RefEnumerate(dims, table, tgt, ref, 256);
    UNIT_CHECK(refCount >= 0);

    UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 256, &count, &singular, NULL));
    UNIT_CHECK(SameRootSet(roots, count, ref, refCount));
    UNIT_CHECK(LexSorted(roots, count));

    // Every published root must re-walk the forward interpolation within tolerance.
    for (i = 0; i < (int) count; i++) {
        cmsFloat32Number fwd[3];
        cmsPipelineEvalFloat(roots[i], fwd, lut);
        for (c = 0; c < 3; c++)
            UNIT_CHECK(fabsf(fwd[c] - target[c]) <= REV_TEST_TOL);
        for (c = 0; c < 3; c++)
            UNIT_CHECK(roots[i][c] >= 0.0f && roots[i][c] <= 1.0f);
    }
}

// ------------------------------------------------------------------------------------------------
// Table shapes.
// ------------------------------------------------------------------------------------------------

static
void IdentityTable(const cmsUInt32Number dims[3], cmsFloat32Number* table)
{
    cmsUInt32Number ix, iy, iz;
    for (ix = 0; ix < dims[0]; ix++)
        for (iy = 0; iy < dims[1]; iy++)
            for (iz = 0; iz < dims[2]; iz++) {
                cmsUInt32Number node = (ix * dims[1] + iy) * dims[2] + iz;
                table[node * 3 + 0] = (cmsFloat32Number) ((double) ix / (dims[0] - 1));
                table[node * 3 + 1] = (cmsFloat32Number) ((double) iy / (dims[1] - 1));
                table[node * 3 + 2] = (cmsFloat32Number) ((double) iz / (dims[2] - 1));
            }
}

// Triangular fold on the y axis: 0 -> 0, middle -> 1, top -> 0. Identity on x and z.
static
void FoldFn(int ix, int iy, int iz, double out[3])
{
    (void) iz;
    out[0] = ix / 2.0;
    out[1] = (iy == 1) ? 1.0 : 0.0;
    out[2] = iz / 2.0;
}

// Non-affine, non folded curved outputs.
static
void CurvedFn(int ix, int iy, int iz, double out[3])
{
    double x = ix / 3.0, y = iy / 3.0, z = iz / 3.0;
    out[0] = x * x;
    out[1] = 0.5 + 0.5 * sin(3.14159265358979323846 * y);
    out[2] = z * (1.0 - z);
}

// Folded and non-affine at the same time.
static
void FoldedCurvedFn(int ix, int iy, int iz, double out[3])
{
    double x = ix / 3.0, y = iy / 3.0, z = iz / 3.0;
    out[0] = (x - 0.5) * (x - 0.5);
    out[1] = y * y;
    out[2] = 0.25 + 0.75 * z;
}

// ================================================================================================
// Group 1: branches, folding, all six tetrahedron directions, boundaries, no-solution, capacity.
// ================================================================================================

void reverse_branches(void)
{
    const cmsUInt32Number dims3[3] = { 3, 3, 3 };
    const cmsUInt32Number dims2[3] = { 2, 2, 2 };
    const cmsUInt32Number dims4[3] = { 4, 4, 4 };
    cmsFloat32Number table[64 * 3];
    cmsPipeline* lut;
    cmsReverseCLUT* idx;
    cmsUInt32Number count, i, c;
    cmsBool singular;
    cmsFloat32Number roots[16][3];

    // --- Folded table with two analytically known roots -------------------------------------
    FillTable(dims3, table, FoldFn);
    lut = MakePipeline(dims3, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);

    {
        cmsFloat32Number target[3] = { 0.25f, 0.5f, 0.75f };
        count = 0; singular = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
        UNIT_CHECK(count == 2);
        UNIT_CHECK(singular == FALSE);
        UNIT_CHECK(LexSorted(roots, count));
        UNIT_CHECK(fabsf(roots[0][0] - 0.25f) <= REV_TEST_TOL);
        UNIT_CHECK(fabsf(roots[0][1] - 0.25f) <= REV_TEST_TOL);
        UNIT_CHECK(fabsf(roots[0][2] - 0.75f) <= REV_TEST_TOL);
        UNIT_CHECK(fabsf(roots[1][0] - 0.25f) <= REV_TEST_TOL);
        UNIT_CHECK(fabsf(roots[1][1] - 0.75f) <= REV_TEST_TOL);
        UNIT_CHECK(fabsf(roots[1][2] - 0.75f) <= REV_TEST_TOL);

        // Count-only call.
        count = 7; singular = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, NULL, 0, &count, &singular, NULL));
        UNIT_CHECK(count == 2);
        UNIT_CHECK(singular == FALSE);

        // Capacity too small: failure, full count, roots untouched.
        for (i = 0; i < 16; i++)
            for (c = 0; c < 3; c++)
                roots[i][c] = -123.0f;
        count = 0; singular = FALSE;
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, target, roots, 1, &count, &singular, NULL));
        UNIT_CHECK(count == 2);
        UNIT_CHECK(singular == FALSE);
        for (i = 0; i < 16; i++)
            for (c = 0; c < 3; c++)
                UNIT_CHECK(roots[i][c] == -123.0f);
    }

    // Fold peak on a shared grid face: exactly one root at y = 0.5.
    {
        cmsFloat32Number target[3] = { 0.25f, 1.0f, 0.75f };
        count = 0; singular = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
        UNIT_CHECK(count == 1);
        UNIT_CHECK(singular == FALSE);
        UNIT_CHECK(fabsf(roots[0][1] - 0.5f) <= REV_TEST_TOL);
    }

    // Unreachable target: success with zero roots, never clamped to a boundary color.
    {
        cmsFloat32Number target[3] = { 0.25f, 1.5f, 0.75f };
        count = 9; singular = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
        UNIT_CHECK(count == 0);
        UNIT_CHECK(singular == FALSE);
    }

    CheckAgainstReference(idx, dims3, table, (cmsFloat32Number[3]){0.1f, 0.2f, 0.3f}, lut);
    CheckAgainstReference(idx, dims3, table, (cmsFloat32Number[3]){0.7f, 0.9f, 0.1f}, lut);
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // --- All six tetrahedron directions in the single cell of a 2x2x2 identity LUT ----------
    IdentityTable(dims2, table);
    lut = MakePipeline(dims2, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);

    {
        // One point per ordering region of (rx, ry, rz).
        const cmsFloat32Number points[6][3] = {
            { 0.7f, 0.5f, 0.3f },   // rx >= ry >= rz
            { 0.7f, 0.3f, 0.5f },   // rx >= rz >= ry
            { 0.5f, 0.3f, 0.7f },   // rz >= rx >= ry
            { 0.5f, 0.7f, 0.3f },   // ry >= rx >= rz
            { 0.3f, 0.7f, 0.5f },   // ry >= rz >= rx
            { 0.3f, 0.5f, 0.7f }    // rz >= ry >= rx
        };
        for (i = 0; i < 6; i++) {
            cmsFloat32Number target[3] = { points[i][0], points[i][1], points[i][2] };
            count = 0; singular = TRUE;
            UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
            UNIT_CHECK(count == 1);
            UNIT_CHECK(singular == FALSE);
            for (c = 0; c < 3; c++)
                UNIT_CHECK(fabsf(roots[0][c] - target[c]) <= REV_TEST_TOL);
            CheckAgainstReference(idx, dims2, table, target, lut);
        }
    }

    // Shared grid faces, edges and vertices collapse to a single preimage.
    {
        const cmsFloat32Number boundary[4][3] = {
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.4f, 0.6f },
            { 0.6f, 0.0f, 0.4f }
        };
        for (i = 0; i < 4; i++) {
            cmsFloat32Number target[3] = { boundary[i][0], boundary[i][1], boundary[i][2] };
            count = 0; singular = TRUE;
            UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
            UNIT_CHECK(count == 1);
            for (c = 0; c < 3; c++)
                UNIT_CHECK(fabsf(roots[0][c] - target[c]) <= REV_TEST_TOL);
        }
    }
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // --- Non-affine curved table, checked against the independent reference -----------------
    FillTable(dims4, table, CurvedFn);
    lut = MakePipeline(dims4, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);

    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.1f, 0.8f, 0.2f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.5f, 0.5f, 0.25f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.9f, 0.2f, 0.05f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.0f, 1.0f, 0.0f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){1.3f, 0.5f, 0.5f}, lut);
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // --- Folded, non-affine table: multiple branches plus reference comparison ---------------
    FillTable(dims4, table, FoldedCurvedFn);
    lut = MakePipeline(dims4, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);

    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.1f, 0.2f, 0.5f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.25f, 0.5f, 0.9f}, lut);
    CheckAgainstReference(idx, dims4, table, (cmsFloat32Number[3]){0.0625f, 0.0f, 1.0f}, lut);
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);
}

// ================================================================================================
// Group 2: singular tetrahedra (point, line, plane collapse and false bounding-box hits).
// ================================================================================================

static
void ConstantFn(int ix, int iy, int iz, double out[3])
{
    (void) ix; (void) iy; (void) iz;
    out[0] = 0.5; out[1] = 0.5; out[2] = 0.5;
}

static
void LineFn(int ix, int iy, int iz, double out[3])
{
    (void) iy; (void) iz;
    out[0] = ix;       // 2-node axis, values 0 and 1; all outputs on the x axis
    out[1] = 0.0;
    out[2] = 0.0;
}

static
void PlaneFn(int ix, int iy, int iz, double out[3])
{
    double x = ix, y = iy;
    (void) iz;
    // Convex hull is the triangle (0,0),(1,0),(0,1); bbox is the unit square.
    out[0] = x;
    out[1] = y * (1.0 - x);
    out[2] = 0.0;
}

static
void QuerySingular(const cmsReverseCLUT* idx, const cmsFloat32Number target[3],
                   cmsBool expectSingular, cmsUInt32Number expectCount)
{
    cmsUInt32Number count = 999;
    cmsBool singular = FALSE;
    cmsFloat32Number roots[16][3];
    UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
    UNIT_CHECK(count == expectCount);
    UNIT_CHECK(singular == expectSingular);
}

void reverse_singular(void)
{
    const cmsUInt32Number dims2[3] = { 2, 2, 2 };
    const cmsUInt32Number dims3[3] = { 3, 3, 3 };
    cmsFloat32Number table[27 * 3];
    cmsPipeline* lut;
    cmsReverseCLUT* idx;

    // Collapse to a single point.
    FillTable(dims2, table, ConstantFn);
    lut = MakePipeline(dims2, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);
    QuerySingular(idx, (cmsFloat32Number[3]){0.5f, 0.5f, 0.5f}, TRUE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){0.500001f, 0.5f, 0.5f}, TRUE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){0.6f, 0.5f, 0.5f}, FALSE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){0.0f, 0.0f, 0.0f}, FALSE, 0);
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // Collapse to a line segment.
    FillTable(dims2, table, LineFn);
    lut = MakePipeline(dims2, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);
    QuerySingular(idx, (cmsFloat32Number[3]){0.5f, 0.0f, 0.0f}, TRUE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){0.0f, 0.0f, 0.0f}, TRUE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){1.0f, 0.0f, 0.0f}, TRUE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){0.5f, 0.001f, 0.0f}, FALSE, 0);
    QuerySingular(idx, (cmsFloat32Number[3]){1.5f, 0.0f, 0.0f}, FALSE, 0);
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // Collapse to a planar triangle, including a point inside the bounding box but
    // outside the convex hull.
    FillTable(dims2, table, PlaneFn);
    lut = MakePipeline(dims2, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);
    QuerySingular(idx, (cmsFloat32Number[3]){0.25f, 0.25f, 0.0f}, TRUE, 0);    // inside hull
    QuerySingular(idx, (cmsFloat32Number[3]){0.0f, 0.0f, 0.0f}, TRUE, 0);       // hull vertex
    QuerySingular(idx, (cmsFloat32Number[3]){0.8f, 0.8f, 0.0f}, FALSE, 0);      // bbox only
    QuerySingular(idx, (cmsFloat32Number[3]){0.9f, 0.9f, 0.001f}, FALSE, 0);    // off the plane
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);

    // Collapse edge (0,0,0)-(0,0,1) onto the origin: the three corner-cell tetrahedra using
    // that edge as an affine direction become singular, the other three stay non-singular.
    IdentityTable(dims3, table);
    UNIT_CHECK(fabsf(table[5] - 0.5f) <= 1e-6f);   // node (0,0,1) output z before collapse
    table[5] = 0.0f;
    lut = MakePipeline(dims3, table);
    UNIT_CHECK(lut != NULL);
    idx = cmsReverseCLUTAlloc(lut);
    UNIT_CHECK(idx != NULL);
    {
        cmsFloat32Number target[3] = { 0.0f, 0.0f, 0.0f };
        cmsFloat32Number roots[16][3];
        cmsUInt32Number count = 0;
        cmsBool singular = FALSE;
        int found = 0, i, c;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 16, &count, &singular, NULL));
        UNIT_CHECK(singular == TRUE);
        UNIT_CHECK(count >= 1);
        for (i = 0; i < (int) count; i++) {
            cmsFloat32Number fwd[3];
            cmsPipelineEvalFloat(roots[i], fwd, lut);
            for (c = 0; c < 3; c++)
                UNIT_CHECK(fabsf(fwd[c] - target[c]) <= REV_TEST_TOL);
            if (fabsf(roots[i][0] - 0.0f) <= REV_TEST_TOL &&
                fabsf(roots[i][1] - 0.0f) <= REV_TEST_TOL &&
                fabsf(roots[i][2] - 0.0f) <= REV_TEST_TOL)
                found = 1;
        }
        UNIT_CHECK(found == 1);
    }
    cmsReverseCLUTFree(idx);
    cmsPipelineFree(lut);
}

// ================================================================================================
// Group 3: index statistics, granular grids, snapshots, interleaving, unsupported pipelines.
// ================================================================================================

// Custom interpolation plug-in used to prove plug-in interpolators are rejected.
static
void FakeFloat3D(const cmsFloat32Number Input[], cmsFloat32Number Output[],
                 const cmsInterpParams* p)
{
    (void) p;
    Output[0] = Input[0];
    Output[1] = Input[1];
    Output[2] = Input[2];
}

static
cmsInterpFunction CustomFactory(cmsUInt32Number nInputChannels,
                                cmsUInt32Number nOutputChannels,
                                cmsUInt32Number dwFlags)
{
    cmsInterpFunction fn;
    memset(&fn, 0, sizeof(fn));
    if (nInputChannels == 3 && nOutputChannels == 3 && (dwFlags & CMS_LERP_FLAGS_FLOAT))
        fn.LerpFloat = FakeFloat3D;
    return fn;
}

static
void GranularFn(int ix, int iy, int iz, double out[3])
{
    (void) iz;
    double x = ix / 2.0;          // dims[0] = 3
    double y = iy / 3.0;          // dims[1] = 4
    double z = iz / 4.0;          // dims[2] = 5
    out[0] = x;
    out[1] = y * y;
    out[2] = (z <= 0.5) ? 2.0 * z : 2.0 * (1.0 - z);
}

void reverse_index(void)
{
    cmsFloat32Number table[33 * 33 * 33 * 3];
    cmsFloat32Number smallTable[64 * 3];
    cmsPipeline* lut;
    cmsReverseCLUT* idx;
    cmsUInt32Number count, work, i;
    cmsBool singular;
    cmsFloat32Number roots[8][3];

    // --- work statistics on the 33^3 identity table -----------------------------------------
    {
        const cmsUInt32Number dims[3] = { 33, 33, 33 };
        cmsUInt32Number nTets = 32u * 32u * 32u * 6u;
        cmsUInt32Number totalWork = 0;
        int a, b, c;

        IdentityTable(dims, table);
        lut = MakePipeline(dims, table);
        UNIT_CHECK(lut != NULL);
        idx = cmsReverseCLUTAlloc(lut);
        UNIT_CHECK(idx != NULL);

        for (a = 0; a < 8; a++)
            for (b = 0; b < 8; b++)
                for (c = 0; c < 8; c++) {
                    int off;
                    // (2i+1)/34 cannot equal a grid face k/32: that would require
                    // 16*(2i+1) = 17*k, impossible for 2i+1 in 1..15.
                    cmsFloat32Number target[3] = {
                        (cmsFloat32Number) ((a + 0.5) / 17.0),
                        (cmsFloat32Number) ((b + 0.5) / 17.0),
                        (cmsFloat32Number) ((c + 0.5) / 17.0)
                    };
                    for (off = 0; off < 3; off++) {
                        double frac = target[off] * 32.0;
                        UNIT_CHECK(fabs(frac - floor(frac + 0.5)) > 1e-9);
                    }
                    count = 0; singular = TRUE; work = 0;
                    UNIT_CHECK(cmsReverseCLUTQuery(idx, target, roots, 8, &count, &singular, &work));
                    UNIT_CHECK(count == 1);
                    UNIT_CHECK(singular == FALSE);
                    UNIT_CHECK(work > 0);
                    UNIT_CHECK(work < nTets / 20);          // no hidden full scan, per query
                    UNIT_CHECK(fabsf(roots[0][0] - target[0]) <= REV_TEST_TOL);
                    UNIT_CHECK(fabsf(roots[0][1] - target[1]) <= REV_TEST_TOL);
                    UNIT_CHECK(fabsf(roots[0][2] - target[2]) <= REV_TEST_TOL);
                    totalWork += work;
                }

        UNIT_CHECK(totalWork / 512u < nTets / 20);          // average requirement

        // An unreachable target must not trigger a table scan either.
        {
            cmsFloat32Number target[3] = { 5.0f, 5.0f, 5.0f };
            work = 12345; count = 9; singular = TRUE;
            UNIT_CHECK(cmsReverseCLUTQuery(idx, target, NULL, 0, &count, &singular, &work));
            UNIT_CHECK(count == 0);
            UNIT_CHECK(singular == FALSE);
            UNIT_CHECK(work < nTets / 20);
        }
        cmsReverseCLUTFree(idx);
        cmsPipelineFree(lut);
    }

    // --- Small-table identity results must match the exhaustive reference -------------------
    {
        const cmsUInt32Number dims[3] = { 3, 3, 3 };
        RefPoint ref[256];
        int a, b, cc;
        IdentityTable(dims, smallTable);
        lut = MakePipeline(dims, smallTable);
        UNIT_CHECK(lut != NULL);
        idx = cmsReverseCLUTAlloc(lut);
        UNIT_CHECK(idx != NULL);

        for (a = 0; a < 4; a++)
            for (b = 0; b < 4; b++)
                for (cc = 0; cc < 4; cc++) {
                    cmsFloat32Number target[3] = {
                        (cmsFloat32Number) ((a + 0.25) / 4.0),
                        (cmsFloat32Number) ((b + 0.5) / 4.0),
                        (cmsFloat32Number) ((cc + 0.75) / 4.0)
                    };
                    double tgt[3] = { target[0], target[1], target[2] };
                    cmsFloat32Number got[16][3];
                    cmsUInt32Number cnt = 0;
                    cmsBool sing = TRUE;
                    int refCount = RefEnumerate(dims, smallTable, tgt, ref, 256);
                    UNIT_CHECK(refCount == 1);
                    UNIT_CHECK(cmsReverseCLUTQuery(idx, target, got, 16, &cnt, &sing, NULL));
                    UNIT_CHECK(cnt == 1);
                    UNIT_CHECK(SameRootSet(got, cnt, ref, refCount));
                }
        cmsReverseCLUTFree(idx);
        cmsPipelineFree(lut);
    }

    // --- Non-uniform axis granularity --------------------------------------------------------
    {
        const cmsUInt32Number dims[3] = { 3, 4, 5 };
        cmsFloat32Number granular[3 * 4 * 5 * 3];
        FillTable(dims, granular, GranularFn);
        lut = MakePipeline(dims, granular);
        UNIT_CHECK(lut != NULL);
        idx = cmsReverseCLUTAlloc(lut);
        UNIT_CHECK(idx != NULL);
        CheckAgainstReference(idx, dims, granular, (cmsFloat32Number[3]){0.5f, 0.3f, 0.4f}, lut);
        CheckAgainstReference(idx, dims, granular, (cmsFloat32Number[3]){0.1f, 0.7f, 0.9f}, lut);
        CheckAgainstReference(idx, dims, granular, (cmsFloat32Number[3]){0.9f, 0.01f, 0.01f}, lut);
        cmsReverseCLUTFree(idx);
        cmsPipelineFree(lut);
    }

    // --- Snapshot survives editing and releasing the source pipeline -------------------------
    {
        const cmsUInt32Number dims[3] = { 4, 4, 4 };
        cmsFloat32Number captured[8][3];
        cmsFloat32Number target[3] = { 0.3f, 0.6f, 0.2f };
        cmsStage* stage;
        _cmsStageCLutData* data;

        FillTable(dims, smallTable, CurvedFn);
        lut = MakePipeline(dims, smallTable);
        UNIT_CHECK(lut != NULL);
        idx = cmsReverseCLUTAlloc(lut);
        UNIT_CHECK(idx != NULL);

        count = 0; singular = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, captured, 8, &count, &singular, NULL));
        UNIT_CHECK(count >= 1);

        // Mutate the source table, then release the pipeline altogether.
        stage = cmsPipelineGetPtrToFirstStage(lut);
        data = (_cmsStageCLutData*) cmsStageData(stage);
        for (i = 0; i < dims[0] * dims[1] * dims[2] * 3; i++)
            data->Tab.TFloat[i] = 0.0f;
        cmsPipelineFree(lut);

        {
            cmsFloat32Number again[8][3];
            cmsUInt32Number count2 = 0;
            cmsBool singular2 = TRUE;
            UNIT_CHECK(cmsReverseCLUTQuery(idx, target, again, 8, &count2, &singular2, NULL));
            UNIT_CHECK(count2 == count);
            UNIT_CHECK(singular2 == singular);
            for (i = 0; i < count; i++) {
                UNIT_CHECK(fabsf(again[i][0] - captured[i][0]) <= REV_TEST_TOL);
                UNIT_CHECK(fabsf(again[i][1] - captured[i][1]) <= REV_TEST_TOL);
                UNIT_CHECK(fabsf(again[i][2] - captured[i][2]) <= REV_TEST_TOL);
            }
        }
        cmsReverseCLUTFree(idx);
    }

    // --- Interleaved queries on two indices do not interfere --------------------------------
    {
        const cmsUInt32Number dims3[3] = { 3, 3, 3 };
        const cmsUInt32Number dims4[3] = { 4, 4, 4 };
        cmsFloat32Number foldTable[27 * 3], curveTable[64 * 3];
        cmsReverseCLUT* idxA;
        cmsReverseCLUT* idxB;
        cmsPipeline* lutA;
        cmsPipeline* lutB;

        FillTable(dims3, foldTable, FoldFn);
        FillTable(dims4, curveTable, CurvedFn);
        lutA = MakePipeline(dims3, foldTable);
        lutB = MakePipeline(dims4, curveTable);
        UNIT_CHECK(lutA != NULL && lutB != NULL);
        idxA = cmsReverseCLUTAlloc(lutA);
        idxB = cmsReverseCLUTAlloc(lutB);
        UNIT_CHECK(idxA != NULL && idxB != NULL);

        for (i = 0; i < 100; i++) {
            cmsFloat32Number tA[3] = { 0.25f, 0.5f, 0.75f };
            cmsFloat32Number tB[3] = { 0.1f + 0.008f * (i % 10), 0.8f, 0.2f };
            cmsFloat32Number rA[8][3], rB[8][3];
            cmsUInt32Number nA = 0, nB = 0, nCount = 0;
            cmsBool sA = TRUE, sB = TRUE;

            UNIT_CHECK(cmsReverseCLUTQuery(idxA, tA, rA, 8, &nA, &sA, NULL));
            UNIT_CHECK(nA == 2 && sA == FALSE);
            UNIT_CHECK(cmsReverseCLUTQuery(idxB, tB, NULL, 0, &nCount, &sB, NULL));
            UNIT_CHECK(nCount >= 1);
            UNIT_CHECK(cmsReverseCLUTQuery(idxB, tB, rB, 8, &nB, &sB, NULL));
            UNIT_CHECK(nB == nCount);
            UNIT_CHECK(fabsf(rA[0][1] - 0.25f) <= REV_TEST_TOL);
            UNIT_CHECK(fabsf(rA[1][1] - 0.75f) <= REV_TEST_TOL);
        }
        cmsReverseCLUTFree(idxA);
        cmsReverseCLUTFree(idxB);
        cmsPipelineFree(lutA);
        cmsPipelineFree(lutB);
    }

    // --- Rejected pipelines ------------------------------------------------------------------
    {
        const cmsUInt32Number dims[3] = { 3, 3, 3 };
        cmsPipeline* p;
        cmsStage* s;
        cmsContext ctx;
        cmsPluginInterpolation plugin = {
            { cmsPluginMagicNumber, 2060, cmsPluginInterpolationSig, NULL },
            CustomFactory
        };

        // 16-bit table.
        p = cmsPipelineAlloc(NULL, 3, 3);
        UNIT_CHECK(p != NULL);
        s = cmsStageAllocCLut16bit(NULL, 3, 3, 3, NULL);
        UNIT_CHECK(s != NULL);
        UNIT_CHECK(cmsPipelineInsertStage(p, cmsAT_END, s));
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        // Trilinear interpolation.
        FillTable(dims, smallTable, CurvedFn);
        p = MakePipeline(dims, smallTable);
        UNIT_CHECK(p != NULL);
        s = cmsPipelineGetPtrToFirstStage(p);
        {
            _cmsStageCLutData* clut = (_cmsStageCLutData*) cmsStageData(s);
            clut->Params->dwFlags |= CMS_LERP_FLAGS_TRILINEAR;
            UNIT_CHECK(_cmsSetInterpolationRoutine(NULL, clut->Params));
        }
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        // Two stages.
        FillTable(dims, smallTable, CurvedFn);
        p = MakePipeline(dims, smallTable);
        UNIT_CHECK(p != NULL);
        s = cmsStageAllocIdentity(NULL, 3);
        UNIT_CHECK(s != NULL);
        UNIT_CHECK(cmsPipelineInsertStage(p, cmsAT_END, s));
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        // Non-finite and out-of-range entries.
        FillTable(dims, smallTable, CurvedFn);
        smallTable[0] = (cmsFloat32Number) INFINITY;
        p = MakePipeline(dims, smallTable);
        UNIT_CHECK(p != NULL);
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        FillTable(dims, smallTable, CurvedFn);
        smallTable[5] = 2.5f;
        p = MakePipeline(dims, smallTable);
        UNIT_CHECK(p != NULL);
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        // Range boundaries are accepted.
        FillTable(dims, smallTable, CurvedFn);
        smallTable[0] = 2.0f;
        smallTable[1] = -2.0f;
        p = MakePipeline(dims, smallTable);
        UNIT_CHECK(p != NULL);
        idx = cmsReverseCLUTAlloc(p);
        UNIT_CHECK(idx != NULL);
        cmsReverseCLUTFree(idx);
        cmsPipelineFree(p);

        // Grid counts out of range.
        {
            const cmsUInt32Number big[3] = { 34, 3, 3 };
            cmsFloat32Number bigTable[34 * 3 * 3 * 3];
            IdentityTable(big, bigTable);
            p = MakePipeline(big, bigTable);
            UNIT_CHECK(p != NULL);
            UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
            cmsPipelineFree(p);
        }

        // Unsupported channel count (4 -> 3).
        p = cmsPipelineAlloc(NULL, 4, 3);
        UNIT_CHECK(p != NULL);
        s = cmsStageAllocCLutFloat(NULL, 2, 4, 3, NULL);
        UNIT_CHECK(s != NULL);
        UNIT_CHECK(cmsPipelineInsertStage(p, cmsAT_END, s));
        UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
        cmsPipelineFree(p);

        // Custom interpolation plug-in.
        ctx = cmsCreateContext(&plugin.base, NULL);
        UNIT_CHECK(ctx != NULL);
        {
            FillTable(dims, smallTable, CurvedFn);
            p = cmsPipelineAlloc(ctx, 3, 3);
            UNIT_CHECK(p != NULL);
            s = cmsStageAllocCLutFloatGranular(ctx, dims, 3, 3, smallTable);
            UNIT_CHECK(s != NULL);
            UNIT_CHECK(cmsPipelineInsertStage(p, cmsAT_END, s));
            UNIT_CHECK(cmsReverseCLUTAlloc(p) == NULL);
            cmsPipelineFree(p);
        }
        cmsDeleteContext(ctx);

        // NULL pipeline and free(NULL).
        UNIT_CHECK(cmsReverseCLUTAlloc(NULL) == NULL);
        cmsReverseCLUTFree(NULL);
    }

    // --- Parameter validation and output handling --------------------------------------------
    {
        const cmsUInt32Number dims[3] = { 3, 3, 3 };
        // All three channels stay inside the range reached by CurvedFn.
        cmsFloat32Number target[3] = { 0.3f, 0.7f, 0.15f };
        cmsFloat32Number nanTarget[3] = { 0.3f, (cmsFloat32Number) NAN, 0.3f };
        cmsFloat32Number infTarget[3] = { 0.3f, 0.3f, (cmsFloat32Number) INFINITY };
        cmsFloat32Number r[4][3];
        cmsUInt32Number n = 7;
        cmsBool sg = TRUE;

        FillTable(dims, smallTable, CurvedFn);
        lut = MakePipeline(dims, smallTable);
        UNIT_CHECK(lut != NULL);
        idx = cmsReverseCLUTAlloc(lut);
        UNIT_CHECK(idx != NULL);

        UNIT_CHECK(!cmsReverseCLUTQuery(idx, target, r, 4, NULL, &sg, NULL));
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, target, r, 4, &n, NULL, NULL));
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, target, NULL, 4, &n, &sg, NULL));
        UNIT_CHECK(!cmsReverseCLUTQuery(NULL, target, r, 4, &n, &sg, NULL));
        UNIT_CHECK(n == 0 && sg == FALSE);

        // Non-NULL roots with zero capacity is a capacity failure, not a write.
        n = 0; sg = FALSE;
        r[0][0] = r[0][1] = r[0][2] = -77.0f;
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, target, r, 0, &n, &sg, NULL));
        UNIT_CHECK(n >= 1 && sg == FALSE);
        UNIT_CHECK(r[0][0] == -77.0f && r[0][1] == -77.0f && r[0][2] == -77.0f);

        n = 7; sg = TRUE; work = 9;
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, nanTarget, r, 4, &n, &sg, &work));
        UNIT_CHECK(n == 0 && sg == FALSE && work == 0);
        n = 7; sg = TRUE; work = 9;
        UNIT_CHECK(!cmsReverseCLUTQuery(idx, infTarget, r, 4, &n, &sg, &work));
        UNIT_CHECK(n == 0 && sg == FALSE && work == 0);

        // work == NULL is allowed and the index/target stay intact.
        n = 0; sg = TRUE;
        UNIT_CHECK(cmsReverseCLUTQuery(idx, target, r, 4, &n, &sg, NULL));
        UNIT_CHECK(n >= 1 && sg == FALSE);

        cmsReverseCLUTFree(idx);
        cmsPipelineFree(lut);
    }
}
