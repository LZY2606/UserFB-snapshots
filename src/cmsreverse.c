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
//  Full-branch reverse index for floating point, 3 -> 3 tetrahedral CLUT pipelines.
//  See the cmsReverseCLUT* documentation in lcms2.h for the supported scope and
//  the numerical contract.
//

#include "lcms2_internal.h"
#include <math.h>
#include <float.h>


// Tolerances mandated by the public contract
#define REV_TOL             2e-6        // L-infinity tolerance for target match and feasibility
#define REV_SINGULAR_TOL    1e-12       // Relative determinant threshold for singular tetrahedra
#define REV_TABLE_LIMIT     2.0         // Table entries must stay within [-2, 2]
#define REV_MAX_GRID        33
#define REV_MIN_GRID        2

// Per-query geometry of the spatial index
#define REV_MIN_BUCKETS     4
#define REV_MAX_BUCKETS     64


// Corner layout, in (x, y, z) grid bits, of the four output vertices q0..q3 in each
// of the six Sakamoto tetrahedra. The order matches TetrahedralInterpFloat() in cmsintrp.c.
static const cmsUInt8Number RevCornerBits[6][4][3] = {
    { {0,0,0}, {1,0,0}, {1,1,0}, {1,1,1} },  // rx >= ry >= rz
    { {0,0,0}, {1,0,0}, {1,1,1}, {1,0,1} },  // rx >= rz >= ry
    { {0,0,0}, {1,0,1}, {1,1,1}, {0,0,1} },  // rz >= rx >= ry
    { {0,0,0}, {1,1,0}, {0,1,0}, {1,1,1} },  // ry >= rx >= rz
    { {0,0,0}, {1,1,1}, {0,1,0}, {0,1,1} },  // ry >= rz >= rx
    { {0,0,0}, {1,1,1}, {0,1,1}, {0,0,1} }   // rz >= ry >= rx
};


struct _cmsReverseCLUT_struct {

    cmsContext ContextID;

    // Independent snapshot of the grid table, interleaved xyz, 3 floats per node.
    cmsFloat32Number* Snapshot;
    cmsUInt32Number   nx, ny, nz;          // Nodes per axis
    cmsUInt32Number   nNodes;
    cmsUInt32Number   nTets;

    // Per tetrahedron: cell origin, branch id (0..5) and its four node indices.
    cmsUInt32Number*  TetCell;             // Packed cell index x0*dy*dz + y0*dz + z0
    cmsUInt8Number*   TetBranch;
    cmsUInt32Number (*TetNodes)[4];

    // Output-space uniform grid used to prune candidate tetrahedra.
    cmsFloat64Number  bboxMin[3], bboxMax[3];
    cmsUInt32Number   nb[3];               // Buckets per axis
    cmsFloat64Number  bucketSize[3];
    cmsUInt32Number*  bucketStart;         // nb[0]*nb[1]*nb[2] + 1 entries
    cmsUInt32Number*  bucketRefs;          // Tetrahedron indices, grouped by bucket

    // Private pipeline reproducing exactly the source forward interpolation.
    cmsPipeline*      Forward;
};


static
cmsBool IsFiniteDouble(cmsFloat64Number v)
{
    return !isnan(v) && !isinf(v);
}


// Map an output-space interval [loVal, hiVal] onto bucket indices, clamped to [0, n].
// A target wholly outside the table range yields an empty, non-wrapping interval.
static
void BucketRange(const cmsReverseCLUT* idx, cmsUInt32Number axis,
                 cmsFloat64Number loVal, cmsFloat64Number hiVal,
                 cmsUInt32Number* loOut, cmsUInt32Number* hiOut)
{
    cmsFloat64Number fLo = (loVal - idx->bboxMin[axis]) / idx->bucketSize[axis];
    cmsFloat64Number fHi = (hiVal - idx->bboxMin[axis]) / idx->bucketSize[axis];
    cmsInt32Number iLo = (cmsInt32Number) floor(fLo);
    cmsInt32Number iHi = (cmsInt32Number) ceil(fHi);

    if (iLo < 0) iLo = 0;
    if (iHi < 0) iHi = 0;
    if (iLo > (cmsInt32Number) idx->nb[axis]) iLo = (cmsInt32Number) idx->nb[axis];
    if (iHi > (cmsInt32Number) idx->nb[axis]) iHi = (cmsInt32Number) idx->nb[axis];

    *loOut = (cmsUInt32Number) iLo;
    *hiOut = (cmsUInt32Number) iHi;
}


// Exact determinant of a 3x3 matrix (columns are the edge vectors).
static
cmsFloat64Number Det3(const cmsFloat64Number a[3][3])
{
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
         - a[1][0] * (a[0][1] * a[2][2] - a[0][2] * a[2][1])
         + a[2][0] * (a[0][1] * a[1][2] - a[0][2] * a[1][1]);
}


// Solve a 3x3 system by Cramer's rule. Returns FALSE if the matrix is (numerically) singular.
static
cmsBool Solve3(const cmsFloat64Number a[3][3], const cmsFloat64Number b[3], cmsFloat64Number x[3])
{
    cmsFloat64Number d = Det3(a), m[3][3];

    if (fabs(d) < DBL_MIN) return FALSE;

    m[0][0] = b[0]; m[0][1] = a[0][1]; m[0][2] = a[0][2];
    m[1][0] = b[1]; m[1][1] = a[1][1]; m[1][2] = a[1][2];
    m[2][0] = b[2]; m[2][1] = a[2][1]; m[2][2] = a[2][2];
    x[0] = Det3(m) / d;

    m[0][0] = a[0][0]; m[0][1] = b[0]; m[0][2] = a[0][2];
    m[1][0] = a[1][0]; m[1][1] = b[1]; m[1][2] = a[1][2];
    m[2][0] = a[2][0]; m[2][1] = b[2]; m[2][2] = a[2][2];
    x[1] = Det3(m) / d;

    m[0][0] = a[0][0]; m[0][1] = a[0][1]; m[0][2] = b[0];
    m[1][0] = a[1][0]; m[1][1] = a[1][1]; m[1][2] = b[1];
    m[2][0] = a[2][0]; m[2][1] = a[2][1]; m[2][2] = b[2];
    x[2] = Det3(m) / d;

    return TRUE;
}


// L-infinity distance from target t to the convex hull of the four output vertices q[0..3].
// The hull is a (possibly degenerate) simplex described by q(u) = q0 + u1*(q1-q0) +
// u2*(q2-q0) + u3*(q3-q0), with u in D = {u >= 0, u1+u2+u3 <= 1}. The objective
// f(u) = max_i |t_i - q(u)_i| is convex and piecewise linear, so its minimum over D is
// attained at a vertex of the arrangement made of the four domain planes and the six
// planes where two coordinate residuals agree (in magnitude and sign). Enumerating every
// triple of those ten planes, plus the four vertices of D, therefore finds the exact
// minimum. The same routine covers tetrahedra collapsed to triangles, quads, lines or
// points without having to determine the rank explicitly.
static
cmsFloat64Number HullLinfDistance(const cmsFloat64Number q[4][3], const cmsFloat64Number t[3])
{
    // Planes a . u = b: 4 domain planes followed by 6 residual-equality planes.
    cmsFloat64Number pa[10][3], pb[10];
    cmsFloat64Number e[3][3], base[3], best;
    int p, i, j, s, c1, c2, c3;

    for (i = 0; i < 3; i++) {
        base[i] = q[0][i];
        e[0][i] = q[1][i] - q[0][i];
        e[1][i] = q[2][i] - q[0][i];
        e[2][i] = q[3][i] - q[0][i];
    }

    // Domain planes: u1 = 0, u2 = 0, u3 = 0, u1+u2+u3 = 1
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) pa[i][j] = (i == j) ? 1.0 : 0.0;
        pb[i] = 0.0;
    }
    for (j = 0; j < 3; j++) pa[3][j] = 1.0;
    pb[3] = 1.0;

    // Residual r_i(u) = (t_i - q0_i) - (e0_i*u1 + e1_i*u2 + e2_i*u3)
    p = 4;
    for (i = 0; i < 3; i++) {
        for (j = i + 1; j < 3; j++) {
            for (s = 0; s < 2; s++) {
                cmsFloat64Number sign = s ? -1.0 : 1.0;   // r_i = sign * r_j
                int k;
                for (k = 0; k < 3; k++)
                    pa[p][k] = e[k][i] - sign * e[k][j];
                pb[p] = (t[i] - base[i]) - sign * (t[j] - base[j]);
                p++;
            }
        }
    }

    // The four vertices of D are always candidates (u = 0 and the three unit vectors).
    best = DBL_MAX;
    {
        static const cmsFloat64Number domainVerts[4][3] = {
            {0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}
        };
        for (c1 = 0; c1 < 4; c1++) {
            cmsFloat64Number dist = 0.0;
            const cmsFloat64Number* u = domainVerts[c1];
            for (i = 0; i < 3; i++) {
                cmsFloat64Number qi = base[i] + e[0][i] * u[0] + e[1][i] * u[1] + e[2][i] * u[2];
                cmsFloat64Number d = fabs(t[i] - qi);
                if (d > dist) dist = d;
            }
            if (dist < best) best = dist;
        }
    }

    // Arrangement vertices: every feasible intersection of three planes.
    for (c1 = 0; c1 < 10; c1++) {
        for (c2 = c1 + 1; c2 < 10; c2++) {
            for (c3 = c2 + 1; c3 < 10; c3++) {
                cmsFloat64Number a[3][3], b[3], u[3], dist;
                a[0][0] = pa[c1][0]; a[0][1] = pa[c1][1]; a[0][2] = pa[c1][2]; b[0] = pb[c1];
                a[1][0] = pa[c2][0]; a[1][1] = pa[c2][1]; a[1][2] = pa[c2][2]; b[1] = pb[c2];
                a[2][0] = pa[c3][0]; a[2][1] = pa[c3][1]; a[2][2] = pa[c3][2]; b[2] = pb[c3];

                if (!Solve3(a, b, u)) continue;

                // Feasibility inside D.
                if (u[0] < -1e-10 || u[1] < -1e-10 || u[2] < -1e-10) continue;
                if (u[0] + u[1] + u[2] > 1.0 + 1e-10) continue;

                dist = 0.0;
                for (i = 0; i < 3; i++) {
                    cmsFloat64Number qi = base[i] + e[0][i] * u[0] + e[1][i] * u[1] + e[2][i] * u[2];
                    cmsFloat64Number d = fabs(t[i] - qi);
                    if (d > dist) dist = d;
                }
                if (dist < best) best = dist;
            }
        }
    }

    return best;
}


// Fetch the four output vertices (in double precision) of a tetrahedron.
static
void TetOutputs(const cmsReverseCLUT* idx, cmsUInt32Number tet, cmsFloat64Number q[4][3])
{
    cmsUInt32Number v, c;
    for (v = 0; v < 4; v++) {
        cmsUInt32Number node = idx->TetNodes[tet][v];
        for (c = 0; c < 3; c++)
            q[v][c] = (cmsFloat64Number) idx->Snapshot[node * 3 + c];
    }
}


// Reconstruct the input point of a tetrahedron from its barycentric weights.
static
void TetInputPoint(const cmsReverseCLUT* idx, cmsUInt32Number tet,
                   const cmsFloat64Number w[4], cmsFloat64Number in[3])
{
    cmsUInt32Number cell = idx->TetCell[tet];
    cmsUInt32Number dz = idx->nz - 1, dy = idx->ny - 1;
    cmsUInt32Number z0 = cell % dz;
    cmsUInt32Number y0 = (cell / dz) % dy;
    cmsUInt32Number x0 = cell / (dy * dz);
    cmsUInt32Number branch = idx->TetBranch[tet];
    int c, v;

    for (c = 0; c < 3; c++) in[c] = 0.0;

    for (v = 0; v < 4; v++) {
        cmsFloat64Number corner[3];
        const cmsUInt8Number* bits = RevCornerBits[branch][v];

        corner[0] = ((cmsFloat64Number) x0 + bits[0]) / ((cmsFloat64Number) idx->nx - 1);
        corner[1] = ((cmsFloat64Number) y0 + bits[1]) / ((cmsFloat64Number) idx->ny - 1);
        corner[2] = ((cmsFloat64Number) z0 + bits[2]) / ((cmsFloat64Number) idx->nz - 1);

        for (c = 0; c < 3; c++)
            in[c] += w[v] * corner[c];
    }
}


// Validate that the pipeline is exactly one float 3->3 CLUT stage using the default
// tetrahedral interpolator, and return the stage data. NULL means unsupported.
static
_cmsStageCLutData* SupportedSingleCLUT(const cmsPipeline* lut, cmsContext* ContextID)
{
    cmsStage* stage;
    _cmsStageCLutData* data;
    _cmsInterpPluginChunkType* interpPlugin;

    if (lut == NULL) return NULL;
    if (cmsPipelineInputChannels(lut) != 3 || cmsPipelineOutputChannels(lut) != 3) return NULL;
    if (cmsPipelineStageCount(lut) != 1) return NULL;

    stage = cmsPipelineGetPtrToFirstStage(lut);
    if (stage == NULL || cmsStageNext(stage) != NULL) return NULL;
    if (cmsStageType(stage) != cmsSigCLutElemType) return NULL;
    if (cmsStageInputChannels(stage) != 3 || cmsStageOutputChannels(stage) != 3) return NULL;

    data = (_cmsStageCLutData*) cmsStageData(stage);
    if (data == NULL || data->Params == NULL) return NULL;

    // Only true floating point tables are accepted.
    if (!data->HasFloatValues || data->Tab.TFloat == NULL) return NULL;
    if (data->Params->nInputs != 3 || data->Params->nOutputs != 3) return NULL;

    // Trilinear, or any plug-in supplied interpolation factory, is out of scope.
    if (data->Params->dwFlags & CMS_LERP_FLAGS_TRILINEAR) return NULL;
    if (data->Params->Interpolation.LerpFloat == NULL) return NULL;

    *ContextID = cmsGetPipelineContextID(lut);
    interpPlugin = (_cmsInterpPluginChunkType*) _cmsContextGetClientChunk(*ContextID, InterpPlugin);
    if (interpPlugin != NULL && interpPlugin->Interpolators != NULL) return NULL;

    return data;
}


// Build the output-space bucket index over the tetrahedron bounding boxes.
static
cmsBool BuildSpatialIndex(cmsContext ContextID, cmsReverseCLUT* idx)
{
    cmsUInt32Number tet, nBuckets, b;
    cmsUInt32Number axis;
    cmsUInt32Number lo[3], hi[3];
    cmsUInt32Number* counts;

    for (axis = 0; axis < 3; axis++) {
        cmsFloat64Number span = idx->bboxMax[axis] - idx->bboxMin[axis];
        cmsUInt32Number n = (cmsUInt32Number) ceil(span / REV_TOL);
        if (n < REV_MIN_BUCKETS) n = REV_MIN_BUCKETS;
        if (n > REV_MAX_BUCKETS) n = REV_MAX_BUCKETS;
        idx->nb[axis] = n;
        idx->bucketSize[axis] = span / (cmsFloat64Number) n;
        if (idx->bucketSize[axis] <= 0.0) idx->bucketSize[axis] = REV_TOL;
    }

    nBuckets = idx->nb[0] * idx->nb[1] * idx->nb[2];
    idx->bucketStart = (cmsUInt32Number*) _cmsCalloc(ContextID, nBuckets + 1, sizeof(cmsUInt32Number));
    if (idx->bucketStart == NULL) return FALSE;

    // First pass: count how many tetrahedra fall in each bucket.
    for (tet = 0; tet < idx->nTets; tet++) {
        cmsFloat64Number q[4][3], mn[3], mx[3];
        TetOutputs(idx, tet, q);

        for (axis = 0; axis < 3; axis++) {
            int v;
            mn[axis] = mx[axis] = q[0][axis];
            for (v = 1; v < 4; v++) {
                if (q[v][axis] < mn[axis]) mn[axis] = q[v][axis];
                if (q[v][axis] > mx[axis]) mx[axis] = q[v][axis];
            }
            // Inflate by a negligible margin so exact boundaries never get lost.
            mn[axis] -= 1e-12;
            mx[axis] += 1e-12;
        }

        for (axis = 0; axis < 3; axis++)
            BucketRange(idx, axis, mn[axis], mx[axis], &lo[axis], &hi[axis]);

        {
            cmsUInt32Number ix, iy, iz;
            for (ix = lo[0]; ix < hi[0]; ix++)
                for (iy = lo[1]; iy < hi[1]; iy++)
                    for (iz = lo[2]; iz < hi[2]; iz++) {
                        b = (ix * idx->nb[1] + iy) * idx->nb[2] + iz;
                        idx->bucketStart[b + 1]++;
                    }
        }
    }

    // Prefix sums.
    for (b = 0; b < nBuckets; b++)
        idx->bucketStart[b + 1] += idx->bucketStart[b];

    idx->bucketRefs = (cmsUInt32Number*) _cmsMalloc(ContextID,
                                                    idx->bucketStart[nBuckets] * sizeof(cmsUInt32Number));
    if (idx->bucketRefs == NULL) return FALSE;

    // Second pass: fill the references using a moving cursor.
    counts = (cmsUInt32Number*) _cmsCalloc(ContextID, nBuckets, sizeof(cmsUInt32Number));
    if (counts == NULL) return FALSE;
    for (tet = 0; tet < idx->nTets; tet++) {
        cmsFloat64Number q[4][3], mn[3], mx[3];
        TetOutputs(idx, tet, q);

        for (axis = 0; axis < 3; axis++) {
            int v;
            mn[axis] = mx[axis] = q[0][axis];
            for (v = 1; v < 4; v++) {
                if (q[v][axis] < mn[axis]) mn[axis] = q[v][axis];
                if (q[v][axis] > mx[axis]) mx[axis] = q[v][axis];
            }
            mn[axis] -= 1e-12;
            mx[axis] += 1e-12;
        }

        for (axis = 0; axis < 3; axis++)
            BucketRange(idx, axis, mn[axis], mx[axis], &lo[axis], &hi[axis]);

        {
            cmsUInt32Number ix, iy, iz;
            for (ix = lo[0]; ix < hi[0]; ix++)
                for (iy = lo[1]; iy < hi[1]; iy++)
                    for (iz = lo[2]; iz < hi[2]; iz++) {
                        b = (ix * idx->nb[1] + iy) * idx->nb[2] + iz;
                        idx->bucketRefs[idx->bucketStart[b] + counts[b]] = tet;
                        counts[b]++;
                    }
        }
    }

    _cmsFree(ContextID, counts);
    return TRUE;
}


cmsReverseCLUT* CMSEXPORT cmsReverseCLUTAlloc(const cmsPipeline* lut)
{
    cmsContext ContextID = NULL;
    _cmsStageCLutData* data;
    cmsReverseCLUT* idx;
    cmsUInt32Number nx, ny, nz, nNodes, nTets;
    cmsUInt32Number x0, y0, z0, tet, node;
    cmsStage* forwardStage;
    cmsUInt32Number dims[3];

    data = SupportedSingleCLUT(lut, &ContextID);
    if (data == NULL) {
        cmsSignalError(ContextID, cmsERROR_NOT_SUITABLE,
                       "Reverse CLUT index requires a single float 3->3 CLUT stage with default tetrahedral interpolation");
        return NULL;
    }

    nx = data->Params->nSamples[0];
    ny = data->Params->nSamples[1];
    nz = data->Params->nSamples[2];
    if (nx < REV_MIN_GRID || nx > REV_MAX_GRID ||
        ny < REV_MIN_GRID || ny > REV_MAX_GRID ||
        nz < REV_MIN_GRID || nz > REV_MAX_GRID) {
        cmsSignalError(ContextID, cmsERROR_RANGE,
                       "Reverse CLUT index grid points must be between 2 and 33 per axis");
        return NULL;
    }

    nNodes = nx * ny * nz;
    if (data->nEntries != nNodes * 3) {
        cmsSignalError(ContextID, cmsERROR_CORRUPTION_DETECTED, "CLUT table size mismatch");
        return NULL;
    }

    // Every table entry must be finite and within [-2, 2].
    for (node = 0; node < nNodes * 3; node++) {
        cmsFloat64Number v = (cmsFloat64Number) data->Tab.TFloat[node];
        if (!IsFiniteDouble(v) || fabs(v) > REV_TABLE_LIMIT) {
            cmsSignalError(ContextID, cmsERROR_RANGE,
                           "Reverse CLUT index only accepts finite table entries within [-2, 2]");
            return NULL;
        }
    }

    idx = (cmsReverseCLUT*) _cmsMallocZero(ContextID, sizeof(cmsReverseCLUT));
    if (idx == NULL) return NULL;
    idx->ContextID = ContextID;
    idx->nx = nx;
    idx->ny = ny;
    idx->nz = nz;
    idx->nNodes = nNodes;

    // Independent snapshot of the table.
    idx->Snapshot = (cmsFloat32Number*) _cmsDupMem(ContextID, data->Tab.TFloat,
                                                   nNodes * 3 * sizeof(cmsFloat32Number));
    if (idx->Snapshot == NULL) goto Error;

    // One tetrahedron set per grid cell, six tetrahedra per cell.
    nTets = (nx - 1) * (ny - 1) * (nz - 1) * 6;
    idx->nTets = nTets;

    idx->TetCell   = (cmsUInt32Number*) _cmsMalloc(ContextID, nTets * sizeof(cmsUInt32Number));
    idx->TetBranch = (cmsUInt8Number*)  _cmsMalloc(ContextID, nTets * sizeof(cmsUInt8Number));
    idx->TetNodes  = (cmsUInt32Number (*)[4]) _cmsMalloc(ContextID, nTets * 4 * sizeof(cmsUInt32Number));
    if (idx->TetCell == NULL || idx->TetBranch == NULL || idx->TetNodes == NULL) goto Error;

    tet = 0;
    for (x0 = 0; x0 < nx - 1; x0++) {
        for (y0 = 0; y0 < ny - 1; y0++) {
            for (z0 = 0; z0 < nz - 1; z0++) {
                int branch;
                for (branch = 0; branch < 6; branch++) {
                    int v;
                    idx->TetCell[tet] = (x0 * (ny - 1) + y0) * (nz - 1) + z0;
                    idx->TetBranch[tet] = (cmsUInt8Number) branch;

                    for (v = 0; v < 4; v++) {
                        cmsUInt32Number ix = x0 + RevCornerBits[branch][v][0];
                        cmsUInt32Number iy = y0 + RevCornerBits[branch][v][1];
                        cmsUInt32Number iz = z0 + RevCornerBits[branch][v][2];
                        idx->TetNodes[tet][v] = (ix * ny + iy) * nz + iz;
                    }
                    tet++;
                }
            }
        }
    }

    // Whole-table output bounding box.
    {
        cmsUInt32Number axis;
        for (axis = 0; axis < 3; axis++) {
            idx->bboxMin[axis] = (cmsFloat64Number) idx->Snapshot[axis];
            idx->bboxMax[axis] = (cmsFloat64Number) idx->Snapshot[axis];
        }
        for (node = 1; node < nNodes; node++) {
            for (axis = 0; axis < 3; axis++) {
                cmsFloat64Number v = (cmsFloat64Number) idx->Snapshot[node * 3 + axis];
                if (v < idx->bboxMin[axis]) idx->bboxMin[axis] = v;
                if (v > idx->bboxMax[axis]) idx->bboxMax[axis] = v;
            }
        }
    }

    if (!BuildSpatialIndex(ContextID, idx)) goto Error;

    // Private forward pipeline over the snapshot, so verification re-walks the exact
    // default tetrahedral interpolation and keeps working after the source is gone.
    dims[0] = nx; dims[1] = ny; dims[2] = nz;
    idx->Forward = cmsPipelineAlloc(ContextID, 3, 3);
    if (idx->Forward == NULL) goto Error;

    forwardStage = cmsStageAllocCLutFloatGranular(ContextID, dims, 3, 3, idx->Snapshot);
    if (forwardStage == NULL) goto Error;
    if (!cmsPipelineInsertStage(idx->Forward, cmsAT_END, forwardStage)) {
        cmsStageFree(forwardStage);
        goto Error;
    }

    return idx;

Error:
    cmsSignalError(ContextID, cmsERROR_INTERNAL, "Cannot build reverse CLUT index");
    cmsReverseCLUTFree(idx);
    return NULL;
}


void CMSEXPORT cmsReverseCLUTFree(cmsReverseCLUT* index)
{
    cmsContext ContextID;

    if (index == NULL) return;
    ContextID = index->ContextID;

    if (index->Forward != NULL) cmsPipelineFree(index->Forward);
    if (index->bucketRefs != NULL) _cmsFree(ContextID, index->bucketRefs);
    if (index->bucketStart != NULL) _cmsFree(ContextID, index->bucketStart);
    if (index->TetNodes != NULL) _cmsFree(ContextID, index->TetNodes);
    if (index->TetBranch != NULL) _cmsFree(ContextID, index->TetBranch);
    if (index->TetCell != NULL) _cmsFree(ContextID, index->TetCell);
    if (index->Snapshot != NULL) _cmsFree(ContextID, index->Snapshot);
    _cmsFree(ContextID, index);
}


// Candidate root gathered during a query.
typedef struct {
    cmsFloat64Number in[3];
} RevCandidate;


static
int CompareCandidates(const void* a, const void* b)
{
    const RevCandidate* ra = (const RevCandidate*) a;
    const RevCandidate* rb = (const RevCandidate*) b;
    int axis;
    for (axis = 0; axis < 3; axis++) {
        if (ra->in[axis] < rb->in[axis]) return -1;
        if (ra->in[axis] > rb->in[axis]) return 1;
    }
    return 0;
}


cmsBool CMSEXPORT cmsReverseCLUTQuery(const cmsReverseCLUT* index,
                                      const cmsFloat32Number target[3],
                                      cmsFloat32Number (*roots)[3],
                                      cmsUInt32Number capacity,
                                      cmsUInt32Number* count,
                                      cmsBool* singular,
                                      cmsUInt32Number* work)
{
    cmsUInt32Number nTets, nCand = 0, nKept = 0;
    cmsUInt32Number i, axis;
    cmsUInt32Number* candidates;
    cmsUInt8Number* tetSeen;
    cmsUInt8Number* bucketSeen;
    RevCandidate* cand;
    cmsUInt32Number nBuckets;
    cmsUInt32Number lo[3], hi[3];
    cmsUInt32Number workBuckets = 0, workRefs = 0;
    cmsBool gotSingular = FALSE;
    cmsFloat64Number tgt[3];

    // count and singular are mandatory outputs.
    if (count == NULL || singular == NULL) {
        if (count != NULL) *count = 0;
        if (singular != NULL) *singular = FALSE;
        if (work != NULL) *work = 0;
        return FALSE;
    }

    // From here on every output pointer except work is known to be valid.
    if (index == NULL || target == NULL || (capacity != 0 && roots == NULL)) {
        *count = 0;
        *singular = FALSE;
        if (work != NULL) *work = 0;
        return FALSE;
    }

    for (axis = 0; axis < 3; axis++) {
        tgt[axis] = (cmsFloat64Number) target[axis];
        if (!IsFiniteDouble(tgt[axis])) {
            *count = 0; *singular = FALSE;
            if (work != NULL) *work = 0;
            return FALSE;
        }
    }

    nTets = index->nTets;
    nBuckets = index->nb[0] * index->nb[1] * index->nb[2];

    candidates = (cmsUInt32Number*) _cmsMalloc(index->ContextID, nTets * sizeof(cmsUInt32Number));
    tetSeen    = (cmsUInt8Number*)  _cmsCalloc(index->ContextID, nTets, sizeof(cmsUInt8Number));
    bucketSeen = (cmsUInt8Number*)  _cmsCalloc(index->ContextID, nBuckets, sizeof(cmsUInt8Number));
    cand       = (RevCandidate*)    _cmsMalloc(index->ContextID, nTets * sizeof(RevCandidate));

    if (candidates == NULL || tetSeen == NULL || bucketSeen == NULL || cand == NULL) {
        if (candidates != NULL) _cmsFree(index->ContextID, candidates);
        if (tetSeen != NULL) _cmsFree(index->ContextID, tetSeen);
        if (bucketSeen != NULL) _cmsFree(index->ContextID, bucketSeen);
        if (cand != NULL) _cmsFree(index->ContextID, cand);
        *count = 0; *singular = FALSE;
        if (work != NULL) *work = 0;
        return FALSE;
    }

    // Output-space cylinder of buckets intersecting the target's tolerance box.
    for (axis = 0; axis < 3; axis++)
        BucketRange(index, axis, tgt[axis] - REV_TOL, tgt[axis] + REV_TOL, &lo[axis], &hi[axis]);

    for (i = lo[0]; i < hi[0]; i++) {
        for (axis = lo[1]; axis < hi[1]; axis++) {
            for (cmsUInt32Number k = lo[2]; k < hi[2]; k++) {
                cmsUInt32Number b = (i * index->nb[1] + axis) * index->nb[2] + k;
                cmsUInt32Number r;

                if (bucketSeen[b]) continue;
                bucketSeen[b] = 1;
                workBuckets++;

                for (r = index->bucketStart[b]; r < index->bucketStart[b + 1]; r++) {
                    cmsUInt32Number tet = index->bucketRefs[r];
                    workRefs++;
                    if (!tetSeen[tet]) {
                        tetSeen[tet] = 1;
                        candidates[nCand++] = tet;
                    }
                }
            }
        }
    }

    // Invert every candidate tetrahedron.
    for (i = 0; i < nCand; i++) {
        cmsUInt32Number tet = candidates[i];
        cmsFloat64Number q[4][3], a[3][3], rhs[3], w[4], in[3];
        cmsFloat64Number scale = 0.0, det;
        int v, c;

        TetOutputs(index, tet, q);

        // Affine edge matrix, columns e1 = q1-q0, e2 = q2-q0, e3 = q3-q0.
        for (c = 0; c < 3; c++) {
            a[c][0] = q[1][c] - q[0][c];
            a[c][1] = q[2][c] - q[0][c];
            a[c][2] = q[3][c] - q[0][c];
            rhs[c] = tgt[c] - q[0][c];
            for (v = 0; v < 3; v++) {
                cmsFloat64Number av = fabs(a[c][v]);
                if (av > scale) scale = av;
            }
        }
        det = Det3(a);

        if (scale == 0.0 || fabs(det) <= REV_SINGULAR_TOL * scale * scale * scale) {
            // Degenerate (or nearly degenerate) tetrahedron: report only when the target
            // is within tolerance of the actual convex hull, not just its bounding box.
            if (HullLinfDistance(q, tgt) <= REV_TOL) gotSingular = TRUE;
            continue;
        }

        // Non-singular: analytic inversion of the affine map.
        if (!Solve3(a, rhs, &w[1])) continue;
        w[0] = 1.0 - w[1] - w[2] - w[3];

        // Feasibility of the barycentric weights, tolerance matching the public contract.
        for (v = 0; v < 4; v++) {
            if (w[v] < -REV_TOL) break;
        }
        if (v < 4) continue;
        for (v = 0; v < 4; v++)
            if (w[v] < 0.0) w[v] = 0.0;

        TetInputPoint(index, tet, w, in);

        // Re-walk the original forward interpolation on the reconstructed input.
        {
            cmsFloat32Number fin[3], fout[3];
            cmsBool inside = TRUE;

            for (c = 0; c < 3; c++) {
                if (in[c] < 0.0 && in[c] >= -REV_TOL) in[c] = 0.0;
                if (in[c] > 1.0 && in[c] <= 1.0 + REV_TOL) in[c] = 1.0;
                if (in[c] < 0.0 || in[c] > 1.0) { inside = FALSE; break; }
                fin[c] = (cmsFloat32Number) in[c];
            }
            if (!inside) continue;

            cmsPipelineEvalFloat(fin, fout, index->Forward);
            for (c = 0; c < 3; c++) {
                if (fabs((cmsFloat64Number) fout[c] - tgt[c]) > REV_TOL) { inside = FALSE; break; }
            }
            if (!inside) continue;
        }

        // Each tetrahedron contributes at most one point, so nKept stays below nTets.
        for (c = 0; c < 3; c++)
            cand[nKept].in[c] = in[c];
        nKept++;
    }

    // Lexicographic sort, then tolerance-based deduplication.
    qsort(cand, nKept, sizeof(RevCandidate), CompareCandidates);

    {
        cmsUInt32Number readIdx, writeIdx = 0;
        for (readIdx = 0; readIdx < nKept; readIdx++) {
            cmsBool duplicate = FALSE;
            for (i = 0; i < writeIdx; i++) {
                cmsFloat64Number d = 0.0;
                for (axis = 0; axis < 3; axis++) {
                    cmsFloat64Number dd = fabs(cand[readIdx].in[axis] - cand[i].in[axis]);
                    if (dd > d) d = dd;
                }
                if (d <= REV_TOL) { duplicate = TRUE; break; }
            }
            if (!duplicate) {
                if (writeIdx != readIdx) cand[writeIdx] = cand[readIdx];
                writeIdx++;
            }
        }
        nKept = writeIdx;
    }

    _cmsFree(index->ContextID, candidates);
    _cmsFree(index->ContextID, tetSeen);
    _cmsFree(index->ContextID, bucketSeen);

    if (work != NULL) *work = workBuckets + workRefs;
    *singular = gotSingular;
    *count = nKept;

    // A counting call (roots == NULL, capacity == 0) always succeeds with the full count.
    // Any call that asked for storage must provide enough room, and roots is left untouched
    // when it does not.
    if (roots != NULL && capacity < nKept) {
        _cmsFree(index->ContextID, cand);
        return FALSE;
    }

    if (roots != NULL) {
        for (i = 0; i < nKept; i++) {
            roots[i][0] = (cmsFloat32Number) cand[i].in[0];
            roots[i][1] = (cmsFloat32Number) cand[i].in[1];
            roots[i][2] = (cmsFloat32Number) cand[i].in[2];
        }
    }

    _cmsFree(index->ContextID, cand);
    return TRUE;
}
