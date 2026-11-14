/* Regression checks for cc_EditPlan.h (sparse edit plans). */
#define CC_EDIT_IMPLEMENTATION
#include "cc_EditPlan.h"
#include "task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* mesh construction helpers                                          */
/* ------------------------------------------------------------------ */

static cc_VertexPoint mkp(float x, float y, float z)
{
    cc_VertexPoint p;

    p.x = x; p.y = y; p.z = z;

    return p;
}

/* Builds a cc_Mesh from a face/vertex description. Edges, twins and
 * mappings are derived automatically; creases default to smooth
 * self-loops. UVs are stored per halfedge. */
static cc_Mesh *def_to_mesh(int32_t vertexCount,
                            const cc_VertexPoint *points,
                            int32_t faceCount,
                            const int32_t *faceSizes,
                            const int32_t *faceVerts)
{
    int32_t halfedgeCount = 0;
    cc_Mesh *m;
    int32_t h = 0;
    int32_t edge = 0;

    for (int32_t f = 0; f < faceCount; ++f) {
        halfedgeCount += faceSizes[f];
    }
    /* edgeCount is an upper bound here; it is fixed after edge pairing */
    m = ccm_Create(vertexCount, halfedgeCount, halfedgeCount, halfedgeCount, faceCount);
    for (int32_t i = 0; i < vertexCount; ++i) {
        m->vertexPoints[i] = points[i];
        m->vertexToHalfedgeIDs[i] = -1;
    }
    for (int32_t f = 0; f < faceCount; ++f) {
        const int32_t n = faceSizes[f];

        m->faceToHalfedgeIDs[f] = h;
        for (int32_t j = 0; j < n; ++j, ++h) {
            cc_Halfedge *e = &m->halfedges[h];

            e->vertexID = faceVerts[h];
            e->faceID = f;
            e->nextID = m->faceToHalfedgeIDs[f] + (j + 1) % n;
            e->prevID = m->faceToHalfedgeIDs[f] + (j + n - 1) % n;
            e->twinID = -1;
            e->edgeID = -1;
            e->uvID = h;
            m->vertexToHalfedgeIDs[e->vertexID] = h;
            m->uvs[h].u = (float)j / (float)n;
            m->uvs[h].v = (float)f / (float)faceCount;
        }
    }
    for (h = 0; h < halfedgeCount; ++h) {
        if (m->halfedges[h].edgeID < 0) {
            const int32_t a = m->halfedges[h].vertexID;
            const int32_t b = m->halfedges[m->halfedges[h].nextID].vertexID;

            for (int32_t t = h + 1; t < halfedgeCount; ++t) {
                if (m->halfedges[t].vertexID == b
                &&  m->halfedges[m->halfedges[t].nextID].vertexID == a) {
                    m->halfedges[h].twinID = t;
                    m->halfedges[t].twinID = h;
                    m->halfedges[h].edgeID = m->halfedges[t].edgeID = edge;
                    break;
                }
            }
            if (m->halfedges[h].edgeID < 0) {
                m->halfedges[h].edgeID = edge; /* boundary halfedge */
            }
            m->edgeToHalfedgeIDs[edge] = h;
            m->creases[edge].nextID = m->creases[edge].prevID = edge;
            m->creases[edge].sharpness = 0.0f;
            ++edge;
        }
    }
    m->edgeCount = edge;

    return m;
}

/* Fills points/faces of an (nx x ny) quad grid at the given offsets. */
static void fill_grid(int32_t nx, int32_t ny,
                      float ox, float oy, float spacing,
                      cc_VertexPoint *points,
                      int32_t *faceSizes,
                      int32_t *faceVerts,
                      int32_t vertexOffset)
{
    int32_t *fv = faceVerts;

    for (int32_t j = 0; j <= ny; ++j) {
        for (int32_t i = 0; i <= nx; ++i) {
            points[vertexOffset + i + (nx + 1) * j] =
                mkp(ox + spacing * (float)i, oy + spacing * (float)j, 0.0f);
        }
    }
    for (int32_t j = 0; j < ny; ++j) {
        for (int32_t i = 0; i < nx; ++i) {
            const int32_t v = vertexOffset + i + (nx + 1) * j;

            *faceSizes++ = 4;
            fv[0] = v;
            fv[1] = v + 1;
            fv[2] = v + 1 + (nx + 1);
            fv[3] = v + (nx + 1);
            fv += 4;
        }
    }
}

static int32_t grid_edge_count(int32_t nx, int32_t ny)
{
    return (nx + 1) * ny + nx * (ny + 1);
}

static cc_Mesh *grid_mesh(int32_t nx, int32_t ny, float ox, float oy, float spacing)
{
    const int32_t nv = (nx + 1) * (ny + 1);
    const int32_t nf = nx * ny;
    cc_VertexPoint *points = (cc_VertexPoint *)malloc(sizeof(*points) * (size_t)nv);
    int32_t *faceSizes = (int32_t *)malloc(sizeof(*faceSizes) * (size_t)nf);
    int32_t *faceVerts = (int32_t *)malloc(sizeof(*faceVerts) * (size_t)(4 * nf));
    cc_Mesh *m;

    fill_grid(nx, ny, ox, oy, spacing, points, faceSizes, faceVerts, 0);
    m = def_to_mesh(nv, points, nf, faceSizes, faceVerts);
    free(points);
    free(faceSizes);
    free(faceVerts);

    return m;
}

static int32_t find_edge(const cc_Mesh *m, int32_t a, int32_t b)
{
    for (int32_t h = 0; h < m->halfedgeCount; ++h) {
        if (m->halfedges[h].vertexID == a
        &&  m->halfedges[m->halfedges[h].nextID].vertexID == b) {
            return m->halfedges[h].edgeID;
        }
    }

    return -1;
}

/* Chains a polyline of edges into a crease of the given sharpness. */
static void set_crease_chain(cc_Mesh *m,
                             const int32_t *vertexPairs,
                             int32_t pairCount,
                             float sharpness)
{
    int32_t prevEdge = -1;

    for (int32_t i = 0; i < pairCount; ++i) {
        const int32_t e = find_edge(m, vertexPairs[2 * i], vertexPairs[2 * i + 1]);

        m->creases[e].sharpness = sharpness;
        if (prevEdge >= 0) {
            m->creases[prevEdge].nextID = e;
            m->creases[e].prevID = prevEdge;
        }
        prevEdge = e;
    }
}

/* Closed mixed-face cage: a cube whose top quad is replaced by four
 * triangles meeting at an apex (quads + triangles, valences 3 and 4). */
static cc_Mesh *closed_mixed_mesh(void)
{
    const cc_VertexPoint points[9] = {
        {-1,-1,-1}, {1,-1,-1}, {-1,1,-1}, {1,1,-1},
        {-1,-1, 1}, {1,-1, 1}, {-1,1, 1}, {1,1, 1},
        {0, 0, 2}
    };
    const int32_t faceSizes[9] = {4, 4, 4, 4, 4, 3, 3, 3, 3};
    const int32_t faceVerts[] = {
        0,2,3,1,  0,1,5,4,  2,6,7,3,  0,4,6,2,  1,3,7,5,
        4,5,8,  5,7,8,  7,6,8,  6,4,8
    };

    return def_to_mesh(9, points, 9, faceSizes, faceVerts);
}

/* Open 4x4 quad grid with a semi-sharp interior crease chain and one
 * sharp boundary edge. */
static cc_Mesh *open_crease_mesh(void)
{
    /* v(i,j) = i + 5 j */
    const int32_t chain[] = {11,12, 12,13}; /* interior polyline */
    const int32_t boundary[] = {0,1};
    cc_Mesh *m = grid_mesh(4, 4, 0.0f, 0.0f, 0.5f);

    set_crease_chain(m, chain, 2, 2.0f);  /* semi-sharp */
    set_crease_chain(m, boundary, 1, 8.0f); /* sharp */

    return m;
}

/* ------------------------------------------------------------------ */
/* comparison helpers                                                 */
/* ------------------------------------------------------------------ */

static int subd_equivalent(const cc_Subd *a, const cc_Subd *b)
{
    const int32_t n = ccs_CumulativeVertexCount(a);

    if (ccs_CumulativeVertexCount(b) != n) {
        return 0;
    }
    for (int32_t i = 0; i < n; ++i) {
        for (int32_t c = 0; c < 3; ++c) {
            const float x = a->vertexPoints[i].array[c];
            const float y = b->vertexPoints[i].array[c];

            if (!(fabsf(x - y) <= 2e-5f * (1.0f + fabsf(y)))) {
                return 0;
            }
        }
    }

    return 1;
}

static int topology_identical(const cc_Subd *a, const cc_Subd *b)
{
    const size_t halfedgeBytes =
        sizeof(cc_Halfedge_SemiRegular) * (size_t)ccs_CumulativeHalfedgeCount(a);
    const size_t creaseBytes =
        sizeof(cc_Crease) * (size_t)ccs_CumulativeCreaseCount(a);

    return memcmp(a->halfedges, b->halfedges, halfedgeBytes) == 0
        && memcmp(a->creases, b->creases, creaseBytes) == 0;
}

/* Compares the plan's subd against a full ccs_Refine_Gather of the
 * latest reference cage, at every depth. */
static int matches_reference(const cc_EditPlan *plan, const cc_Mesh *refCage, int32_t depth)
{
    const cc_Subd *got = ccs_EditPlanSubd(plan);
    cc_Subd *ref = ccs_Create(refCage, depth);
    int ok;

    if (!ref) {
        return 0;
    }
    ccs_Refine_Gather(ref);
    ok = subd_equivalent(got, ref) && topology_identical(got, ref);
    ccs_Release(ref);

    return ok;
}

static void apply_edit(cc_Mesh *cage,
                       const int32_t *ids,
                       const cc_VertexPoint *positions,
                       size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        cage->vertexPoints[ids[i]] = positions[i];
    }
}

static size_t stats_evaluated_sum(const cc_EditStats *st)
{
    size_t s = 0;

    for (int i = 0; i < 5; ++i) {
        s += st->evaluated_rows[i];
    }

    return s;
}

static size_t stats_visited_sum(const cc_EditStats *st)
{
    size_t s = 0;

    for (int i = 0; i < 5; ++i) {
        s += st->visited_rows[i];
    }

    return s;
}

static int stats_all_zero(const cc_EditStats *st)
{
    for (int i = 0; i < 5; ++i) {
        if (st->evaluated_rows[i] != 0 || st->visited_rows[i] != 0) {
            return 0;
        }
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* equivalence                                                        */
/* ------------------------------------------------------------------ */

static int equiv_closed_mixed_sequence(void)
{
    cc_Mesh *cage = closed_mixed_mesh();
    cc_Mesh *refCage = closed_mixed_mesh();
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 5);
    cc_EditStats st;

    TASK_CHECK(plan != NULL);
    TASK_CHECK(ccs_EditPlanSubd(plan) != NULL);
    TASK_CHECK(ccs_MaxDepth(ccs_EditPlanSubd(plan)) == 5);
    TASK_CHECK(matches_reference(plan, refCage, 5));

    /* a sequence of accumulating edits, checked against a full
     * recompute of the latest reference cage after each step */
    {
        const int32_t ids[] = {0};
        const cc_VertexPoint pos[] = {{2.0f, -1.5f, 0.3f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) == 0);
        apply_edit(refCage, ids, pos, 1);
        TASK_CHECK(stats_evaluated_sum(&st) > 0);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        const int32_t ids[] = {8};
        const cc_VertexPoint pos[] = {{0.4f, 0.1f, 3.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) == 0);
        apply_edit(refCage, ids, pos, 1);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        const int32_t ids[] = {1, 2, 3};
        const cc_VertexPoint pos[] = {
            {1.5f, -0.25f, -1.0f},
            {-0.75f, 1.25f, -0.5f},
            {0.9f, 0.8f, -1.4f}
        };

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 3, &st) == 0);
        apply_edit(refCage, ids, pos, 3);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        /* rewriting the current position produces no geometric work */
        const int32_t ids[] = {0};
        const cc_VertexPoint pos[] = {{2.0f, -1.5f, 0.3f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) == 0);
        TASK_CHECK(stats_all_zero(&st));
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        const int32_t ids[] = {4, 5, 6, 7, 8};
        const cc_VertexPoint pos[] = {
            {-1.2f, -0.9f, 1.1f},
            {0.8f, -1.1f, 0.6f},
            {-0.4f, 1.3f, 1.2f},
            {1.1f, 0.7f, 0.9f},
            {0.0f, 0.0f, 1.0f}
        };

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 5, &st) == 0);
        apply_edit(refCage, ids, pos, 5);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }

    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    ccm_Release(refCage);

    return 0;
}

static int equiv_open_crease_sequence(void)
{
    cc_Mesh *cage = open_crease_mesh();
    cc_Mesh *refCage = open_crease_mesh();
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 5);
    cc_EditStats st;

    TASK_CHECK(plan != NULL);
    TASK_CHECK(matches_reference(plan, refCage, 5));

    {
        /* corner vertex (boundary + sharp edge endpoint) */
        const int32_t ids[] = {0};
        const cc_VertexPoint pos[] = {{-0.5f, -0.25f, 0.4f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) == 0);
        apply_edit(refCage, ids, pos, 1);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        /* vertices on the semi-sharp crease chain */
        const int32_t ids[] = {11, 12, 13};
        const cc_VertexPoint pos[] = {
            {0.5f, 1.1f, 0.35f},
            {0.6f, 0.9f, -0.2f},
            {0.7f, 1.2f, 0.1f}
        };

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 3, &st) == 0);
        apply_edit(refCage, ids, pos, 3);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        /* interior smooth vertex and a boundary vertex */
        const int32_t ids[] = {7, 20};
        const cc_VertexPoint pos[] = {
            {0.4f, 0.6f, 0.8f},
            {0.1f, 2.2f, -0.3f}
        };

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, &st) == 0);
        apply_edit(refCage, ids, pos, 2);
        TASK_CHECK(matches_reference(plan, refCage, 5));
    }
    {
        /* move everything back to the original grid */
        cc_Mesh *fresh = open_crease_mesh();
        int32_t ids[25];
        cc_VertexPoint pos[25];

        for (int32_t i = 0; i < 25; ++i) {
            ids[i] = i;
            pos[i] = fresh->vertexPoints[i];
        }
        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 25, &st) == 0);
        apply_edit(refCage, ids, pos, 25);
        TASK_CHECK(matches_reference(plan, refCage, 5));
        ccm_Release(fresh);
    }

    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    ccm_Release(refCage);

    return 0;
}

static int equiv_depth_sweep(void)
{
    /* one edit per supported depth, compared against the reference */
    for (int32_t depth = 1; depth <= 5; ++depth) {
        cc_Mesh *cage = closed_mixed_mesh();
        cc_Mesh *refCage = closed_mixed_mesh();
        cc_EditPlan *plan = ccs_EditPlanCreate(cage, depth);
        const int32_t ids[] = {2, 8};
        const cc_VertexPoint pos[] = {{-0.3f, 1.4f, -0.6f}, {0.2f, -0.1f, 2.5f}};

        TASK_CHECK(plan != NULL);
        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, NULL) == 0);
        apply_edit(refCage, ids, pos, 2);
        TASK_CHECK(matches_reference(plan, refCage, depth));
        ccs_EditPlanRelease(plan);
        ccm_Release(cage);
        ccm_Release(refCage);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* locality                                                           */
/* ------------------------------------------------------------------ */

enum {
    LOC_NX = 32, LOC_NY = 32, /* component A: 32x32 quads */
    LOC_MX = 6,  LOC_MY = 6   /* component B: disconnected 6x6 grid */
};

static int row_in_component_b(int32_t level,
                              int32_t row,
                              int32_t VA, int32_t FA, int32_t EA,
                              int32_t V0, int32_t F0, int32_t E0, int32_t H0)
{
    const int32_t V1 = V0 + E0 + F0;

    if (level == 1) {
        if (row >= VA && row < V0) return 1;                 /* B vertices */
        if (row >= V0 + FA && row < V0 + F0) return 1;       /* B face points */
        if (row >= V0 + F0 + EA && row < V1) return 1;       /* B edge points */
        return 0;
    }
    if (row < V1) {
        return row_in_component_b(1, row, VA, FA, EA, V0, F0, E0, H0);
    }
    {
        const int32_t HA = 4 * FA;
        const int32_t F1 = H0;
        const int32_t e = row - (V1 + F1);

        if (row >= V1 + HA && row < V1 + F1) return 1;       /* B face points */
        if (e >= 2 * EA && e < 2 * E0) return 1;             /* B edge points */
        if (e >= 2 * E0 + HA && e < 2 * E0 + H0) return 1;
    }

    return 0;
}

static int loc_grid_drag(void)
{
    const int32_t VA = (LOC_NX + 1) * (LOC_NY + 1);
    const int32_t VB = (LOC_MX + 1) * (LOC_MY + 1);
    const int32_t FA = LOC_NX * LOC_NY;
    const int32_t FB = LOC_MX * LOC_MY;
    const int32_t V0 = VA + VB;
    const int32_t F0 = FA + FB;
    const int32_t E0 = grid_edge_count(LOC_NX, LOC_NY) + grid_edge_count(LOC_MX, LOC_MY);
    const int32_t EA = grid_edge_count(LOC_NX, LOC_NY);
    const int32_t H0 = 4 * F0;
    cc_VertexPoint *points =
        (cc_VertexPoint *)malloc(sizeof(*points) * (size_t)V0);
    int32_t *faceSizes = (int32_t *)malloc(sizeof(*faceSizes) * (size_t)F0);
    int32_t *faceVerts = (int32_t *)malloc(sizeof(*faceVerts) * (size_t)H0);
    cc_Mesh *cage, *refCage;
    cc_EditPlan *plan;
    const cc_Subd *subd;
    cc_VertexPoint *snapshot;
    cc_EditStats st;
    int32_t total;
    int32_t id;
    cc_VertexPoint pos;

    fill_grid(LOC_NX, LOC_NY, 0.0f, 0.0f, 1.0f,
              points, faceSizes, faceVerts, 0);
    fill_grid(LOC_MX, LOC_MY, 1000.0f, 1000.0f, 1.0f,
              points, faceSizes + FA, faceVerts + 4 * FA, VA);
    cage = def_to_mesh(V0, points, F0, faceSizes, faceVerts);
    refCage = def_to_mesh(V0, points, F0, faceSizes, faceVerts);
    free(points);
    free(faceSizes);
    free(faceVerts);

    plan = ccs_EditPlanCreate(cage, 2);
    TASK_CHECK(plan != NULL);
    subd = ccs_EditPlanSubd(plan);
    total = ccs_CumulativeVertexCount(subd);
    snapshot = (cc_VertexPoint *)malloc(sizeof(*snapshot) * (size_t)total);
    memcpy(snapshot, subd->vertexPoints, sizeof(*snapshot) * (size_t)total);

    /* drag a single interior vertex of component A */
    id = 17 + 17 * (LOC_NX + 1);
    pos = mkp(17.0f, 17.0f, 0.75f);
    TASK_CHECK(ccs_EditPlanUpdate(plan, &id, &pos, 1, &st) == 0);

    /* nonzero but local work: visited candidates stay well below a
     * quarter of the total position row count */
    TASK_CHECK(stats_evaluated_sum(&st) > 0);
    TASK_CHECK(stats_visited_sum(&st) > 0);
    TASK_CHECK(stats_evaluated_sum(&st) < (size_t)total / 4);
    TASK_CHECK(stats_visited_sum(&st) < (size_t)total / 4);

    /* the disconnected component B is byte-identical at every depth */
    subd = ccs_EditPlanSubd(plan);
    for (int32_t depth = 1; depth <= 2; ++depth) {
        const int32_t rows = ccm_VertexCountAtDepth(cage, depth);

        for (int32_t r = 0; r < rows; ++r) {
            if (row_in_component_b(depth, r, VA, FA, EA, V0, F0, E0, H0)) {
                const int32_t stride = ccs_CumulativeVertexCountAtDepth(cage, depth - 1);

                TASK_CHECK(memcmp(&subd->vertexPoints[stride + r],
                                  &snapshot[stride + r],
                                  sizeof(cc_VertexPoint)) == 0);
            }
        }
    }

    /* the edit is still exactly equivalent to a full recompute */
    apply_edit(refCage, &id, &pos, 1);
    TASK_CHECK(matches_reference(plan, refCage, 2));

    /* a no-change update reports zero work */
    TASK_CHECK(ccs_EditPlanUpdate(plan, &id, &pos, 1, &st) == 0);
    TASK_CHECK(stats_all_zero(&st));

    /* dragging component B leaves component A untouched */
    memcpy(snapshot, subd->vertexPoints, sizeof(*snapshot) * (size_t)total);
    id = VA + 3 + 3 * (LOC_MX + 1);
    pos = mkp(1003.0f, 1003.0f, -0.5f);
    TASK_CHECK(ccs_EditPlanUpdate(plan, &id, &pos, 1, &st) == 0);
    TASK_CHECK(stats_evaluated_sum(&st) > 0);
    subd = ccs_EditPlanSubd(plan);
    for (int32_t depth = 1; depth <= 2; ++depth) {
        const int32_t rows = ccm_VertexCountAtDepth(cage, depth);

        for (int32_t r = 0; r < rows; ++r) {
            if (!row_in_component_b(depth, r, VA, FA, EA, V0, F0, E0, H0)) {
                const int32_t stride = ccs_CumulativeVertexCountAtDepth(cage, depth - 1);

                TASK_CHECK(memcmp(&subd->vertexPoints[stride + r],
                                  &snapshot[stride + r],
                                  sizeof(cc_VertexPoint)) == 0);
            }
        }
    }
    apply_edit(refCage, &id, &pos, 1);
    TASK_CHECK(matches_reference(plan, refCage, 2));

    free(snapshot);
    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    ccm_Release(refCage);

    return 0;
}

/* ------------------------------------------------------------------ */
/* ownership                                                          */
/* ------------------------------------------------------------------ */

static int own_source_release(void)
{
    cc_Mesh *cage = closed_mixed_mesh();
    cc_Mesh *refCage = closed_mixed_mesh();
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 3);

    TASK_CHECK(plan != NULL);

    /* the source cage is released right after creation */
    ccm_Release(cage);

    {
        const int32_t ids[] = {3, 8};
        const cc_VertexPoint pos[] = {{1.2f, 0.4f, -0.7f}, {0.1f, 0.2f, 2.8f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, NULL) == 0);
        apply_edit(refCage, ids, pos, 2);
        TASK_CHECK(matches_reference(plan, refCage, 3));
    }
    {
        const int32_t ids[] = {0, 1, 2};
        const cc_VertexPoint pos[] = {
            {-0.8f, -1.2f, -0.5f},
            {0.6f, -0.4f, -1.1f},
            {-1.3f, 0.9f, -0.8f}
        };

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 3, NULL) == 0);
        apply_edit(refCage, ids, pos, 3);
        TASK_CHECK(matches_reference(plan, refCage, 3));
    }

    ccs_EditPlanRelease(plan);
    ccm_Release(refCage);

    return 0;
}

static int own_dual_plan(void)
{
    cc_Mesh *cage = closed_mixed_mesh();
    cc_Mesh *refCage1 = closed_mixed_mesh();
    cc_Mesh *refCage2 = closed_mixed_mesh();
    cc_EditPlan *plan1 = ccs_EditPlanCreate(cage, 2);
    cc_EditPlan *plan2 = ccs_EditPlanCreate(cage, 4);

    TASK_CHECK(plan1 != NULL && plan2 != NULL);

    /* interleaved drags on two independent plans */
    {
        const int32_t ids[] = {0};
        const cc_VertexPoint pos[] = {{1.5f, -0.5f, 0.2f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan1, ids, pos, 1, NULL) == 0);
        apply_edit(refCage1, ids, pos, 1);
    }
    {
        const int32_t ids[] = {5, 8};
        const cc_VertexPoint pos[] = {{0.3f, -1.4f, 1.6f}, {-0.2f, 0.3f, 1.5f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan2, ids, pos, 2, NULL) == 0);
        apply_edit(refCage2, ids, pos, 2);
    }
    {
        const int32_t ids[] = {4};
        const cc_VertexPoint pos[] = {{-0.6f, -0.6f, 0.6f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan1, ids, pos, 1, NULL) == 0);
        apply_edit(refCage1, ids, pos, 1);
    }
    {
        const int32_t ids[] = {8};
        const cc_VertexPoint pos[] = {{0.0f, 0.0f, 3.5f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan2, ids, pos, 1, NULL) == 0);
        apply_edit(refCage2, ids, pos, 1);
    }

    /* each plan matches its own reference only */
    TASK_CHECK(matches_reference(plan1, refCage1, 2));
    TASK_CHECK(matches_reference(plan2, refCage2, 4));

    ccs_EditPlanRelease(plan1);
    ccs_EditPlanRelease(plan2);
    ccm_Release(cage);
    ccm_Release(refCage1);
    ccm_Release(refCage2);

    return 0;
}

static int own_reject_then_resume(void)
{
    cc_Mesh *cage = open_crease_mesh();
    cc_Mesh *refCage = open_crease_mesh();
    cc_EditPlan *plan;
    const cc_Subd *subd;
    cc_VertexPoint *snapshot;
    int32_t total;
    cc_EditStats st;

    /* invalid creations */
    TASK_CHECK(ccs_EditPlanCreate(NULL, 2) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, 0) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, 6) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, -1) == NULL);

    plan = ccs_EditPlanCreate(cage, 3);
    TASK_CHECK(plan != NULL);
    subd = ccs_EditPlanSubd(plan);
    total = ccs_CumulativeVertexCount(subd);
    snapshot = (cc_VertexPoint *)malloc(sizeof(*snapshot) * (size_t)total);
    memcpy(snapshot, subd->vertexPoints, sizeof(*snapshot) * (size_t)total);

    /* rejected batches must not move any position */
    {
        const int32_t ids[] = {25}; /* out of range (V0 == 25) */
        const cc_VertexPoint pos[] = {{0.0f, 0.0f, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) != 0);
    }
    {
        const int32_t ids[] = {-1};
        const cc_VertexPoint pos[] = {{0.0f, 0.0f, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) != 0);
    }
    {
        const int32_t ids[] = {3};
        const cc_VertexPoint pos[] = {{NAN, 0.0f, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) != 0);
    }
    {
        const int32_t ids[] = {3};
        const cc_VertexPoint pos[] = {{0.0f, INFINITY, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) != 0);
    }
    {
        const int32_t ids[] = {2, 2}; /* duplicate ids */
        const cc_VertexPoint pos[] = {{0.1f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, &st) != 0);
    }
    {
        const int32_t ids[] = {1};
        const cc_VertexPoint pos[] = {{0.1f, 0.0f, 0.0f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, NULL, pos, 1, &st) != 0);
        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, NULL, 1, &st) != 0);
        (void)ids;
    }
    TASK_CHECK(memcmp(subd->vertexPoints, snapshot,
                      sizeof(*snapshot) * (size_t)total) == 0);

    /* an empty batch may pass NULL arrays and succeeds with zero work */
    TASK_CHECK(ccs_EditPlanUpdate(plan, NULL, NULL, 0, &st) == 0);
    TASK_CHECK(stats_all_zero(&st));

    /* valid updates still accumulate on the last successful state */
    {
        const int32_t ids[] = {6, 12};
        const cc_VertexPoint pos[] = {{0.3f, 0.7f, 0.5f}, {0.9f, 1.1f, -0.4f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, &st) == 0);
        apply_edit(refCage, ids, pos, 2);
        TASK_CHECK(matches_reference(plan, refCage, 3));
    }
    {
        const int32_t ids[] = {6};
        const cc_VertexPoint pos[] = {{0.8f, 0.2f, 0.1f}};

        TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 1, &st) == 0);
        apply_edit(refCage, ids, pos, 1);
        TASK_CHECK(matches_reference(plan, refCage, 3));
    }

    free(snapshot);
    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    ccm_Release(refCage);

    return 0;
}

/* ------------------------------------------------------------------ */
/* case table                                                         */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"equivalence", "closed_mixed_sequence", equiv_closed_mixed_sequence},
    {"equivalence", "open_crease_sequence", equiv_open_crease_sequence},
    {"equivalence", "depth_sweep", equiv_depth_sweep},
    {"locality", "grid_drag_locality", loc_grid_drag},
    {"ownership", "source_release", own_source_release},
    {"ownership", "dual_plan_interleave", own_dual_plan},
    {"ownership", "reject_then_resume", own_reject_then_resume},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
