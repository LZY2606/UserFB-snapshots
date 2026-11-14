//
// Regression contracts for the voxelizer.
//
// Defines VOXELIZER_IMPLEMENTATION with counting allocation hooks and provides
// four contract groups, each returning the number of assertions executed
// (negative on failure):
//
//  - contract_integration: analytic area / first-moment checks of the clipped
//    color integration on slanted triangles and a rectangle
//  - contract_partition:   full occupancy sets and colors for the witness
//    geometries, re-triangulation, negative coordinates, unequal voxel sizes
//    and inflated boxes, checked against independent geometric oracles
//  - contract_outputs:     point cloud / cube mesh / uncolored path
//    correspondence, plus the preserved occupancy of an ordinary model
//  - contract_ownership:   duplicate keys, hash collisions and allocation
//    balance under custom VX_MALLOC/VX_CALLOC/VX_FREE hooks
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Counting allocation hooks (also exercised by contract_ownership)

static long g_alloc_balance = 0;

static void* t_malloc(size_t n)
{
    void* p = malloc(n);
    if (p) { g_alloc_balance++; }
    return p;
}

static void* t_calloc(size_t count, size_t size)
{
    void* p = calloc(count, size);
    if (p) { g_alloc_balance++; }
    return p;
}

static void t_free(void* p)
{
    if (p) {
        g_alloc_balance--;
        free(p);
    }
}

#define VOXELIZER_HELPERS 1
#define VX_MIN(a, b) ((a) > (b) ? (b) : (a))
#define VX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define VX_FINDMINMAX(x0, x1, x2, min, max) \
    min = max = x0;                         \
    if (x1 < min) min = x1;                 \
    if (x1 > max) max = x1;                 \
    if (x2 < min) min = x2;                 \
    if (x2 > max) max = x2;
#define VX_CLAMP(v, lo, hi) VX_MAX(lo, VX_MIN(hi, v))
#define VX_MALLOC(T, N) ((T*) t_malloc((N) * sizeof(T)))
#define VX_FREE(T) t_free(T)
#define VX_CALLOC(T, N) ((T*) t_calloc((N), sizeof(T)))
#define VX_SWAP(T, A, B) { T tmp = B; B = A; A = tmp; }
#define VX_ASSERT(STMT) do { if (!(STMT)) { abort(); } } while (0)

#define VOXELIZER_IMPLEMENTATION
#include "voxelizer.h"

// ---------------------------------------------------------------------------
// Test helpers (scratch memory here uses plain malloc/free and is always
// released inside the helper's scope, so it never affects g_alloc_balance)

#define CHECK(cond) do {                                            \
    n++;                                                            \
    if (!(cond)) {                                                  \
        fprintf(stderr, "CHECK failed %s:%d: %s\n",                 \
                __FILE__, __LINE__, #cond);                         \
        return -n;                                                  \
    }                                                               \
} while (0)

static vx_mesh_t* make_mesh(size_t nv, size_t ni,
    const float* verts,
    const float* cols,
    const unsigned int* ids)
{
    vx_mesh_t* m = vx_mesh_alloc((int) nv, (int) ni);

    for (size_t i = 0; i < nv; ++i) {
        m->vertices[i].x = verts[i*3+0];
        m->vertices[i].y = verts[i*3+1];
        m->vertices[i].z = verts[i*3+2];
        if (cols) {
            m->colors[i].r = cols[i*3+0];
            m->colors[i].g = cols[i*3+1];
            m->colors[i].b = cols[i*3+2];
        }
    }

    if (!cols) {
        VX_FREE(m->colors);
        m->colors = NULL;
    }

    for (size_t i = 0; i < ni; ++i) {
        m->indices[i] = ids[i];
    }

    return m;
}

// Finds the point cloud entry whose integer lattice key is (kx, ky, kz)
static int pc_find(const vx_point_cloud_t* pc,
    double sx, double sy, double sz,
    long long kx, long long ky, long long kz)
{
    for (size_t i = 0; i < pc->nvertices; ++i) {
        if (llround((double) pc->vertices[i].x / sx) == kx &&
            llround((double) pc->vertices[i].y / sy) == ky &&
            llround((double) pc->vertices[i].z / sz) == kz) {
            return (int) i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Independent geometric oracles (not derived from the product's clipping or
// area code)

// Witness geometry 1: triangle conv{(3,0,0),(0,3,0),(0,0,3)}, unit voxels,
// precision 2.5, so intersection boxes are [k-3, k+3]^3. A closed box meets
// the triangle iff it meets the positive octant and its sum range brackets 3.
static int oracle_witness1(long long kx, long long ky, long long kz,
    int* area_positive)
{
    long long k[3] = {kx, ky, kz};
    long long lo = 0, hi = 0;
    int strict_axes = 1;

    for (int i = 0; i < 3; ++i) {
        long long l;
        if (k[i] < -3 || k[i] > 6) { return 0; }
        l = k[i] - 3 > 0 ? k[i] - 3 : 0;
        if (!(l < k[i] + 3)) { strict_axes = 0; }
        lo += l;
        hi += k[i] + 3;
    }

    if (!(lo <= 3 && 3 <= hi)) { return 0; }

    if (area_positive) {
        *area_positive = strict_axes && lo < 3 && 3 < hi;
    }

    return 1;
}

// Ordinary model: surface of the cube [-c, c]^3 with c = 0.7f, voxel size
// 0.25, precision 0. A cell is occupied iff its closed box meets one of the
// six face rectangles.
static int oracle_cube(long long kx, long long ky, long long kz)
{
    const double c = (double) 0.7f;
    const double h = 0.125;
    double lo[3], hi[3];

    lo[0] = 0.25 * (double) kx - h; hi[0] = 0.25 * (double) kx + h;
    lo[1] = 0.25 * (double) ky - h; hi[1] = 0.25 * (double) ky + h;
    lo[2] = 0.25 * (double) kz - h; hi[2] = 0.25 * (double) kz + h;

    for (int axis = 0; axis < 3; ++axis) {
        int o1 = (axis + 1) % 3;
        int o2 = (axis + 2) % 3;
        for (int side = 0; side < 2; ++side) {
            double f = side ? c : -c;
            if (lo[axis] > f || hi[axis] < f) { continue; }
            if (lo[o1] > c || hi[o1] < -c) { continue; }
            if (lo[o2] > c || hi[o2] < -c) { continue; }
            return 1;
        }
    }

    return 0;
}

// Witness geometry meshes: A=(3,0,0), B=(0,3,0), C=(0,0,3), optionally split
// through the interior point P=(1,1,1) into ABP, BCP, CAP. Colors follow the
// linear field C(p) = p/3 (P takes the field value). variant 1 reverses the
// winding of every triangle, variant 2 rotates the corners cyclically.
static vx_mesh_t* witness_mesh(int split, float scale, int variant)
{
    const float base[4][3] = {{3,0,0},{0,3,0},{0,0,3},{1,1,1}};
    const float cbase[4][3] = {{1,0,0},{0,1,0},{0,0,1},
                               {1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f}};
    unsigned int ids[9];
    float verts[12];
    float cols[12];
    int nv = split ? 4 : 3;
    int ni = split ? 9 : 3;

    for (int i = 0; i < nv; ++i) {
        for (int j = 0; j < 3; ++j) {
            verts[i*3+j] = base[i][j] * scale;
            cols[i*3+j] = cbase[i][j];
        }
    }

    if (!split) {
        ids[0] = 0; ids[1] = 1; ids[2] = 2;
    } else {
        ids[0] = 0; ids[1] = 1; ids[2] = 3;
        ids[3] = 1; ids[4] = 2; ids[5] = 3;
        ids[6] = 2; ids[7] = 0; ids[8] = 3;
    }

    for (int t = 0; t < ni / 3; ++t) {
        unsigned int a = ids[t*3+0], b = ids[t*3+1], c = ids[t*3+2];
        if (variant == 1) {
            ids[t*3+0] = c; ids[t*3+1] = b; ids[t*3+2] = a;
        } else if (variant == 2) {
            ids[t*3+0] = b; ids[t*3+1] = c; ids[t*3+2] = a;
        }
    }

    return make_mesh((size_t) nv, (size_t) ni, verts, cols, ids);
}

// Ordinary model: cube [-0.7, 0.7]^3 as 12 triangles
static vx_mesh_t* cube_mesh(int colored)
{
    const float c = 0.7f;
    const float v[8][3] = {
        {-c,-c,-c},{c,-c,-c},{c,c,-c},{-c,c,-c},
        {-c,-c,c},{c,-c,c},{c,c,c},{-c,c,c}
    };
    const unsigned int idx[36] = {
        0,2,1, 0,3,2,  4,5,6, 4,6,7,  0,1,5, 0,5,4,
        2,3,7, 2,7,6,  1,2,6, 1,6,5,  0,4,7, 0,7,3
    };
    float verts[24];
    float cols[24];

    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 3; ++j) {
            verts[i*3+j] = v[i][j];
        }
        cols[i*3+0] = 0.25f * (float) i / 8.0f;
        cols[i*3+1] = 0.5f;
        cols[i*3+2] = 1.0f - 0.1f * (float) i;
    }

    return make_mesh(8, 36, verts, colored ? cols : NULL, idx);
}

// ---------------------------------------------------------------------------

int contract_integration(void)
{
    int n = 0;

    // Slanted triangle (0,0,0)-(2,0,0)-(0,2,0) in the z=0 plane with the
    // linear color field C(p) = (x, y, 0). The intersection of the triangle
    // with every unit cell box is a polygon whose area and first moments are
    // computed analytically below (no product code involved).
    {
        const float verts[9] = {0,0,0, 2,0,0, 0,2,0};
        const float cols[9]  = {0,0,0, 2,0,0, 0,2,0};
        const unsigned int ids[3] = {0,1,2};
        static const struct { long long k[3]; double c[3]; } exp[8] = {
            {{0,0,0}, {0.25, 0.25, 0.0}},        // [0,0.5]^2
            {{1,0,0}, {1.0, 0.25, 0.0}},         // [0.5,1.5]x[0,0.5]
            {{2,0,0}, {5.0/3.0, 1.0/6.0, 0.0}},  // triangle (1.5,0),(2,0),(1.5,0.5)
            {{0,1,0}, {0.25, 1.0, 0.0}},
            {{1,1,0}, {5.0/6.0, 5.0/6.0, 0.0}},  // triangle (0.5,0.5),(1.5,0.5),(0.5,1.5)
            {{0,2,0}, {1.0/6.0, 5.0/3.0, 0.0}},
            {{2,1,0}, {0.0, 0.0, 0.0}},          // point contact at (1.5,0.5)
            {{1,2,0}, {0.0, 0.0, 0.0}},          // point contact at (0.5,1.5)
        };
        vx_mesh_t* m = make_mesh(3, 3, verts, cols, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 1.0f, 1.0f, 0.0f);

        CHECK(pc->nvertices == 8);
        CHECK(pc->colors != NULL);

        for (int e = 0; e < 8; ++e) {
            int idx = pc_find(pc, 1.0, 1.0, 1.0,
                    exp[e].k[0], exp[e].k[1], exp[e].k[2]);
            CHECK(idx >= 0);
            if (exp[e].c[0] == 0.0 && exp[e].c[1] == 0.0 && exp[e].c[2] == 0.0) {
                // zero-area contact: numerically black, no division by zero
                CHECK(pc->colors[idx].r == 0.0f);
                CHECK(pc->colors[idx].g == 0.0f);
                CHECK(pc->colors[idx].b == 0.0f);
            } else {
                CHECK(fabs((double) pc->colors[idx].r - exp[e].c[0]) < 1e-6);
                CHECK(fabs((double) pc->colors[idx].g - exp[e].c[1]) < 1e-6);
                CHECK(fabs((double) pc->colors[idx].b - exp[e].c[2]) < 1e-6);
            }
        }

        // linear colors outside [0, 1] are integrated, not clamped
        {
            int idx = pc_find(pc, 1.0, 1.0, 1.0, 2, 0, 0);
            CHECK(idx >= 0);
            CHECK(pc->colors[idx].r > 1.6f && pc->colors[idx].r < 1.7f);
        }

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    // Rectangle [0,2]^2 in the z=0 plane split into two triangles along the
    // diagonal, same linear field C(p) = (x, y, 0); per-cell integrals are
    // again analytic
    {
        const float verts[12] = {0,0,0, 2,0,0, 2,2,0, 0,2,0};
        const float cols[12]  = {0,0,0, 2,0,0, 2,2,0, 0,2,0};
        const unsigned int ids[6] = {0,1,2, 0,2,3};
        const double mx[3] = {0.25, 1.0, 1.75};
        vx_mesh_t* m = make_mesh(4, 6, verts, cols, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 1.0f, 1.0f, 0.0f);

        CHECK(pc->nvertices == 9);

        for (long long i = 0; i < 3; ++i) {
            for (long long j = 0; j < 3; ++j) {
                int idx = pc_find(pc, 1.0, 1.0, 1.0, i, j, 0);
                CHECK(idx >= 0);
                CHECK(fabs((double) pc->colors[idx].r - mx[i]) < 1e-6);
                CHECK(fabs((double) pc->colors[idx].g - mx[j]) < 1e-6);
                CHECK(pc->colors[idx].b == 0.0f);
            }
        }

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    // Red and blue surfaces covering different areas of the same cell: the
    // cell color is the surface-area weighted mix, not a first-hit or a 50/50
    {
        const float verts[18] = {
            0,0,0, 2,0,0, 0,2,0,                 // red triangle
            -0.5f,0,0, 0,-0.5f,0, -0.5f,-0.5f,0  // blue triangle
        };
        const float cols[18] = {
            1,0,0, 1,0,0, 1,0,0,
            0,0,1, 0,0,1, 0,0,1
        };
        const unsigned int ids[6] = {0,1,2, 3,4,5};
        vx_mesh_t* m = make_mesh(6, 6, verts, cols, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 1.0f, 1.0f, 0.0f);
        int idx;

        CHECK(pc->nvertices == 11);

        // cell (0,0,0): red area 0.25, blue area 0.125
        idx = pc_find(pc, 1.0, 1.0, 1.0, 0, 0, 0);
        CHECK(idx >= 0);
        CHECK(fabs((double) pc->colors[idx].r - 2.0/3.0) < 1e-6);
        CHECK(pc->colors[idx].g == 0.0f);
        CHECK(fabs((double) pc->colors[idx].b - 1.0/3.0) < 1e-6);

        // cell (2,0,0): red only
        idx = pc_find(pc, 1.0, 1.0, 1.0, 2, 0, 0);
        CHECK(idx >= 0);
        CHECK(pc->colors[idx].r == 1.0f);
        CHECK(pc->colors[idx].g == 0.0f);
        CHECK(pc->colors[idx].b == 0.0f);

        // cell (-1,0,0): edge-only contact with the blue triangle, black
        idx = pc_find(pc, 1.0, 1.0, 1.0, -1, 0, 0);
        CHECK(idx >= 0);
        CHECK(pc->colors[idx].r == 0.0f);
        CHECK(pc->colors[idx].g == 0.0f);
        CHECK(pc->colors[idx].b == 0.0f);

        // no contact at all one cell further out
        CHECK(pc_find(pc, 1.0, 1.0, 1.0, 3, 0, 0) < 0);
        CHECK(pc_find(pc, 1.0, 1.0, 1.0, -2, 0, 0) < 0);

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    // A thin sliver of intersection still produces a finite, correct color
    {
        const float verts[9] = {0,0,0, 4,0,0, 0,0.001f,0};
        const float cols[9] = {
            0.3f,0.6f,0.9f, 0.3f,0.6f,0.9f, 0.3f,0.6f,0.9f
        };
        const unsigned int ids[3] = {0,1,2};
        vx_mesh_t* m = make_mesh(3, 3, verts, cols, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 1.0f, 1.0f, 0.0f);
        int idx = pc_find(pc, 1.0, 1.0, 1.0, 0, 0, 0);

        CHECK(pc->nvertices > 0);
        CHECK(idx >= 0);
        CHECK(isfinite(pc->colors[idx].r));
        CHECK(isfinite(pc->colors[idx].g));
        CHECK(isfinite(pc->colors[idx].b));
        CHECK(fabs((double) pc->colors[idx].r - 0.3) < 1e-6);
        CHECK(fabs((double) pc->colors[idx].g - 0.6) < 1e-6);
        CHECK(fabs((double) pc->colors[idx].b - 0.9) < 1e-6);

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    return n;
}

// ---------------------------------------------------------------------------

int contract_partition(void)
{
    int n = 0;

    // Witness geometry 1: scale 1, unit voxels, precision 2.5. The full cell
    // set is checked against the independent oracle in both directions; the
    // original triangle, the 3-way split through P, a reversed winding and a
    // cyclic corner rotation must all agree.
    {
        vx_mesh_t* m0 = witness_mesh(0, 1.0f, 0);
        vx_mesh_t* m1 = witness_mesh(1, 1.0f, 0);
        vx_mesh_t* m2 = witness_mesh(0, 1.0f, 1);
        vx_mesh_t* m3 = witness_mesh(1, 1.0f, 2);
        vx_point_cloud_t* p0 = vx_voxelize_pc(m0, 1.0f, 1.0f, 1.0f, 2.5f);
        vx_point_cloud_t* p1 = vx_voxelize_pc(m1, 1.0f, 1.0f, 1.0f, 2.5f);
        vx_point_cloud_t* p2 = vx_voxelize_pc(m2, 1.0f, 1.0f, 1.0f, 2.5f);
        vx_point_cloud_t* p3 = vx_voxelize_pc(m3, 1.0f, 1.0f, 1.0f, 2.5f);
        size_t oracle_count = 0;

        for (long long kx = -3; kx <= 6; ++kx) {
            for (long long ky = -3; ky <= 6; ++ky) {
                for (long long kz = -3; kz <= 6; ++kz) {
                    if (oracle_witness1(kx, ky, kz, NULL)) {
                        oracle_count++;
                        CHECK(pc_find(p0, 1.0, 1.0, 1.0, kx, ky, kz) >= 0);
                        CHECK(pc_find(p1, 1.0, 1.0, 1.0, kx, ky, kz) >= 0);
                    }
                }
            }
        }

        CHECK(oracle_count == 838);
        CHECK(p0->nvertices == oracle_count);
        CHECK(p1->nvertices == oracle_count);
        CHECK(p2->nvertices == oracle_count);
        CHECK(p3->nvertices == oracle_count);

        // every emitted cell is inside the oracle set, and the cell center
        // (3,3,3) is present in both triangulations
        CHECK(pc_find(p0, 1.0, 1.0, 1.0, 3, 3, 3) >= 0);
        CHECK(pc_find(p1, 1.0, 1.0, 1.0, 3, 3, 3) >= 0);

        for (size_t i = 0; i < p0->nvertices; ++i) {
            long long kx = llround((double) p0->vertices[i].x);
            long long ky = llround((double) p0->vertices[i].y);
            long long kz = llround((double) p0->vertices[i].z);
            int area_positive = 0;
            int i1, i2, i3;

            CHECK(oracle_witness1(kx, ky, kz, &area_positive));

            i1 = pc_find(p1, 1.0, 1.0, 1.0, kx, ky, kz);
            i2 = pc_find(p2, 1.0, 1.0, 1.0, kx, ky, kz);
            i3 = pc_find(p3, 1.0, 1.0, 1.0, kx, ky, kz);
            CHECK(i1 >= 0 && i2 >= 0 && i3 >= 0);

            if (area_positive) {
                CHECK(fabs((double) p0->colors[i].r - p1->colors[i1].r) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].g - p1->colors[i1].g) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].b - p1->colors[i1].b) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].r - p2->colors[i2].r) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].g - p2->colors[i2].g) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].b - p2->colors[i2].b) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].r - p3->colors[i3].r) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].g - p3->colors[i3].g) <= 1e-5);
                CHECK(fabs((double) p0->colors[i].b - p3->colors[i3].b) <= 1e-5);
            } else {
                // zero-area contact: numerically black in every variant
                CHECK(p0->colors[i].r == 0.0f && p0->colors[i].g == 0.0f &&
                      p0->colors[i].b == 0.0f);
                CHECK(p1->colors[i1].r == 0.0f && p1->colors[i1].g == 0.0f &&
                      p1->colors[i1].b == 0.0f);
                CHECK(p2->colors[i2].r == 0.0f && p2->colors[i2].g == 0.0f &&
                      p2->colors[i2].b == 0.0f);
                CHECK(p3->colors[i3].r == 0.0f && p3->colors[i3].g == 0.0f &&
                      p3->colors[i3].b == 0.0f);
            }
        }

        vx_point_cloud_free(p0);
        vx_point_cloud_free(p1);
        vx_point_cloud_free(p2);
        vx_point_cloud_free(p3);
        vx_mesh_free(m0);
        vx_mesh_free(m1);
        vx_mesh_free(m2);
        vx_mesh_free(m3);
    }

    // Witness geometry 2: the same triangle scaled by 0.00015, precision 0.
    // The split triangles are tiny but have positive area and must not vanish.
    {
        vx_mesh_t* m0 = witness_mesh(0, 0.00015f, 0);
        vx_mesh_t* m1 = witness_mesh(1, 0.00015f, 0);
        vx_point_cloud_t* p0 = vx_voxelize_pc(m0, 1.0f, 1.0f, 1.0f, 0.0f);
        vx_point_cloud_t* p1 = vx_voxelize_pc(m1, 1.0f, 1.0f, 1.0f, 0.0f);
        int i0, i1;

        CHECK(p0->nvertices == 1);
        CHECK(p1->nvertices == 1);

        i0 = pc_find(p0, 1.0, 1.0, 1.0, 0, 0, 0);
        i1 = pc_find(p1, 1.0, 1.0, 1.0, 0, 0, 0);
        CHECK(i0 >= 0 && i1 >= 0);

        // the whole triangle lies inside the single cell, so the cell color
        // is the field value at the centroid: (1/3, 1/3, 1/3)
        CHECK(fabs((double) p0->colors[i0].r - 1.0/3.0) < 1e-6);
        CHECK(fabs((double) p0->colors[i0].g - 1.0/3.0) < 1e-6);
        CHECK(fabs((double) p0->colors[i0].b - 1.0/3.0) < 1e-6);
        CHECK(fabs((double) p0->colors[i0].r - p1->colors[i1].r) <= 1e-5);
        CHECK(fabs((double) p0->colors[i0].g - p1->colors[i1].g) <= 1e-5);
        CHECK(fabs((double) p0->colors[i0].b - p1->colors[i1].b) <= 1e-5);

        vx_point_cloud_free(p0);
        vx_point_cloud_free(p1);
        vx_mesh_free(m0);
        vx_mesh_free(m1);
    }

    // Negative coordinates, unequal voxel sizes and inflated boxes: a tiny
    // positive-area triangle around p = (-2.3, 1.7, -0.4) with voxel size
    // (1,2,3) and precision 0.75 (half widths 1.25, 1.75, 2.25)
    {
        const float px = -2.3f, py = 1.7f, pz = -0.4f;
        const float verts[9] = {
            px, py, pz, px + 1e-4f, py, pz, px, py + 1e-4f, pz
        };
        const float cols[9] = {
            0.1f,0.2f,0.3f, 0.1f,0.2f,0.3f, 0.1f,0.2f,0.3f
        };
        const unsigned int ids[3] = {0,1,2};
        static const long long expk[4][3] = {
            {-3,0,0}, {-3,1,0}, {-2,0,0}, {-2,1,0}
        };
        vx_mesh_t* m = make_mesh(3, 3, verts, cols, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 2.0f, 3.0f, 0.75f);

        CHECK(pc->nvertices == 4);

        for (int e = 0; e < 4; ++e) {
            int idx = pc_find(pc, 1.0, 2.0, 3.0,
                    expk[e][0], expk[e][1], expk[e][2]);
            CHECK(idx >= 0);
            CHECK(fabs((double) pc->colors[idx].r - 0.1) < 1e-6);
            CHECK(fabs((double) pc->colors[idx].g - 0.2) < 1e-6);
            CHECK(fabs((double) pc->colors[idx].b - 0.3) < 1e-6);
        }

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    // Truly degenerate triangles (collinear, repeated vertices, single point)
    // are skipped entirely, on the colored and the uncolored path alike
    {
        const float verts[27] = {
            0,0,0, 1,1,1, 2,2,2,    // collinear
            5,5,5, 5,5,5, 6,6,6,    // repeated vertex
            7,7,7, 7,7,7, 7,7,7     // single point
        };
        const unsigned int ids[9] = {0,1,2, 3,4,5, 6,7,8};
        vx_mesh_t* m = make_mesh(9, 9, verts, NULL, ids);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 1.0f, 1.0f, 1.0f, 0.5f);

        CHECK(pc->nvertices == 0);
        CHECK(pc->colors == NULL);

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    return n;
}

// ---------------------------------------------------------------------------

int contract_outputs(void)
{
    int n = 0;

    // Ordinary model with precision = 0: the occupancy of the previous
    // implementation (218 cells for this cube) is preserved, and the full
    // set matches the independent face-rectangle oracle
    {
        vx_mesh_t* m = cube_mesh(0);
        vx_point_cloud_t* pc = vx_voxelize_pc(m, 0.25f, 0.25f, 0.25f, 0.0f);

        CHECK(pc->nvertices == 218);
        CHECK(pc->colors == NULL);

        for (long long kx = -4; kx <= 4; ++kx) {
            for (long long ky = -4; ky <= 4; ++ky) {
                for (long long kz = -4; kz <= 4; ++kz) {
                    int expect = oracle_cube(kx, ky, kz);
                    int got = pc_find(pc, 0.25, 0.25, 0.25, kx, ky, kz) >= 0;
                    CHECK(expect == got);
                }
            }
        }

        vx_point_cloud_free(pc);
        vx_mesh_free(m);
    }

    // Colored cube: point cloud, cube mesh and uncolored path must agree cell
    // by cell, and the cube mesh follows the fixed vertex/index/normal
    // conventions
    {
        const float off[8][3] = {
            {-1, 1, 1}, {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1},
            {-1, 1,-1}, {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1},
        };
        vx_mesh_t* mu = cube_mesh(0);
        vx_mesh_t* mc = cube_mesh(1);
        vx_point_cloud_t* pu = vx_voxelize_pc(mu, 0.25f, 0.25f, 0.25f, 0.0f);
        vx_point_cloud_t* pc = vx_voxelize_pc(mc, 0.25f, 0.25f, 0.25f, 0.0f);
        vx_mesh_t* cubes = vx_voxelize(mc, 0.25f, 0.25f, 0.25f, 0.0f);
        size_t nvox = pc->nvertices;

        CHECK(pu->nvertices == nvox);
        CHECK(pu->colors == NULL);
        CHECK(pc->colors != NULL);
        CHECK(cubes->nvertices == 8 * nvox);
        CHECK(cubes->nindices == 36 * nvox);
        CHECK(cubes->nnormals == 6);
        CHECK(cubes->colors != NULL);

        for (int i = 0; i < 6; ++i) {
            CHECK(cubes->normals[i].x == vx_normals[i*3+0]);
            CHECK(cubes->normals[i].y == vx_normals[i*3+1]);
            CHECK(cubes->normals[i].z == vx_normals[i*3+2]);
        }

        // uncolored and colored paths produce the same occupancy set
        for (size_t i = 0; i < nvox; ++i) {
            long long kx = llround((double) pc->vertices[i].x / 0.25);
            long long ky = llround((double) pc->vertices[i].y / 0.25);
            long long kz = llround((double) pc->vertices[i].z / 0.25);
            CHECK(pc_find(pu, 0.25, 0.25, 0.25, kx, ky, kz) >= 0);
        }
        for (size_t i = 0; i < pu->nvertices; ++i) {
            long long kx = llround((double) pu->vertices[i].x / 0.25);
            long long ky = llround((double) pu->vertices[i].y / 0.25);
            long long kz = llround((double) pu->vertices[i].z / 0.25);
            CHECK(pc_find(pc, 0.25, 0.25, 0.25, kx, ky, kz) >= 0);
        }

        // per-cell correspondence between the cube mesh and the point cloud
        for (size_t j = 0; j < nvox; ++j) {
            double center[3] = {0.0, 0.0, 0.0};
            long long kx, ky, kz;
            int idx;

            for (int c = 0; c < 8; ++c) {
                center[0] += cubes->vertices[j*8+c].x;
                center[1] += cubes->vertices[j*8+c].y;
                center[2] += cubes->vertices[j*8+c].z;
            }
            center[0] /= 8.0;
            center[1] /= 8.0;
            center[2] /= 8.0;

            kx = llround(center[0] / 0.25);
            ky = llround(center[1] / 0.25);
            kz = llround(center[2] / 0.25);

            idx = pc_find(pc, 0.25, 0.25, 0.25, kx, ky, kz);
            CHECK(idx >= 0);

            CHECK(fabs(center[0] - (double) pc->vertices[idx].x) < 1e-6);
            CHECK(fabs(center[1] - (double) pc->vertices[idx].y) < 1e-6);
            CHECK(fabs(center[2] - (double) pc->vertices[idx].z) < 1e-6);

            for (int c = 0; c < 8; ++c) {
                // eight corners at center +/- half voxel, fixed convention
                CHECK(fabs((double) cubes->vertices[j*8+c].x -
                       (center[0] + 0.125 * off[c][0])) < 1e-6);
                CHECK(fabs((double) cubes->vertices[j*8+c].y -
                       (center[1] + 0.125 * off[c][1])) < 1e-6);
                CHECK(fabs((double) cubes->vertices[j*8+c].z -
                       (center[2] + 0.125 * off[c][2])) < 1e-6);
                // every corner carries the point cloud cell color
                CHECK(fabs((double) cubes->colors[j*8+c].r -
                       (double) pc->colors[idx].r) < 1e-6);
                CHECK(fabs((double) cubes->colors[j*8+c].g -
                       (double) pc->colors[idx].g) < 1e-6);
                CHECK(fabs((double) cubes->colors[j*8+c].b -
                       (double) pc->colors[idx].b) < 1e-6);
            }

            for (int t = 0; t < 36; ++t) {
                CHECK(cubes->indices[j*36+t] ==
                      vx_voxel_indices[t] + (unsigned int) (8 * j));
                CHECK(cubes->normalindices[j*36+t] == vx_normal_indices[t/6]);
            }
        }

        vx_mesh_free(cubes);
        vx_point_cloud_free(pc);
        vx_point_cloud_free(pu);
        vx_mesh_free(mc);
        vx_mesh_free(mu);
    }

    return n;
}

// ---------------------------------------------------------------------------

int contract_ownership(void)
{
    int n = 0;
    long base = g_alloc_balance;

    for (int rep = 0; rep < 3; ++rep) {
        // Large plane: ~5000 cells in a 4096 bucket table, forcing real hash
        // collisions; its two triangles share many cells (duplicate keys)
        const float verts1[18] = {
            0,0,0.33f, 8,0,0.33f, 0,6,0.33f,
            8,0,0.33f, 8,6,0.33f, 0,6,0.33f
        };
        const float cols1[18] = {
            0,1,0, 0,1,0, 0,1,0,
            0,1,0, 0,1,0, 0,1,0
        };
        const unsigned int ids1[6] = {0,1,2, 3,4,5};

        // Coincident red/blue triangles: duplicate keys accumulate, the cell
        // color is the area weighted mix of both surfaces
        const float verts2[18] = {
            0.1f,0.1f,0.33f, 0.9f,0.1f,0.33f, 0.1f,0.9f,0.33f,
            0.1f,0.1f,0.33f, 0.9f,0.1f,0.33f, 0.1f,0.9f,0.33f
        };
        const float cols2[18] = {
            1,0,0, 1,0,0, 1,0,0,
            0,0,1, 0,0,1, 0,0,1
        };
        const unsigned int ids2[6] = {0,1,2, 3,4,5};

        vx_mesh_t* m1 = make_mesh(6, 6, verts1, cols1, ids1);
        vx_mesh_t* m2 = make_mesh(6, 6, verts2, cols2, ids2);
        vx_point_cloud_t* pc1 = vx_voxelize_pc(m1, 0.1f, 0.1f, 0.1f, 0.0f);
        vx_mesh_t* cubes1 = vx_voxelize(m1, 0.1f, 0.1f, 0.1f, 0.0f);
        vx_point_cloud_t* pc2 = vx_voxelize_pc(m2, 0.1f, 0.1f, 0.1f, 0.0f);
        int idx;

        CHECK(pc1->nvertices > 4096);
        CHECK(cubes1->nvertices == 8 * pc1->nvertices);
        CHECK(cubes1->nindices == 36 * pc1->nvertices);

        idx = pc_find(pc2, 0.1, 0.1, 0.1, 1, 1, 3);
        CHECK(idx >= 0);
        CHECK(fabs((double) pc2->colors[idx].r - 0.5) < 1e-6);
        CHECK(pc2->colors[idx].g == 0.0f);
        CHECK(fabs((double) pc2->colors[idx].b - 0.5) < 1e-6);

        vx_point_cloud_free(pc2);
        vx_mesh_free(cubes1);
        vx_point_cloud_free(pc1);
        vx_mesh_free(m2);
        vx_mesh_free(m1);

        // every allocation came back
        CHECK(g_alloc_balance == base);
    }

    return n;
}
