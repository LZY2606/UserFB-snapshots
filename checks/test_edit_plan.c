/*
 * checks/test_edit_plan.c -- regression tests for the sparse cc_EditPlan.
 *
 * Groups:
 *   equivalence - sparse updates match a full ccs_Refine_Gather reference at
 *                 every subdivision depth, on closed mixed-face cages and on
 *                 open-boundary cages with semi-sharp creases.
 *   locality    - a single drag on a large planar grid only touches a local
 *                 footprint; a disconnected component is never visited.
 *   ownership   - source release, interleaved plans, rejected batches and
 *                 allocation-failure cleanup.
 */
#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocation-failure injection for the plan's own allocator. */
static long cce_test_live;
static long cce_test_fail_after = -1; /* -1: never fail */

static void *cce_test_malloc(size_t size)
{
    if (cce_test_fail_after >= 0) {
        if (cce_test_fail_after == 0) {
            return NULL;
        }
        cce_test_fail_after -= 1;
    }
    void *ptr = malloc(size);
    if (ptr != NULL) {
        cce_test_live += 1;
    }
    return ptr;
}

static void cce_test_free(void *ptr)
{
    if (ptr != NULL) {
        cce_test_live -= 1;
    }
    free(ptr);
}

#define CCE_MALLOC(x) cce_test_malloc(x)
#define CCE_FREE(x)   cce_test_free(x)
#define CC_EDIT_IMPLEMENTATION
#include "cc_EditPlan.h"

#include "task_support/fixture.h"

/* TASK_CHECK returns the int status code; pointer-returning builders need NULL. */
#define REQUIRE(x) do { if (!task_check(!!(x), #x, __FILE__, __LINE__)) return NULL; } while (0)

/* ------------------------------------------------------------------ */
/* Test mesh construction                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t count;
    const int32_t *vertices;
} cce__Face;

static cc_Mesh *cce__DeepCopy(const cc_Mesh *src)
{
    cc_Mesh *dst = ccm_Create(src->vertexCount, src->uvCount, src->halfedgeCount,
                              src->edgeCount, src->faceCount);
    REQUIRE(dst != NULL);
    memcpy(dst->vertexToHalfedgeIDs, src->vertexToHalfedgeIDs,
           (size_t)src->vertexCount * sizeof(int32_t));
    memcpy(dst->edgeToHalfedgeIDs, src->edgeToHalfedgeIDs,
           (size_t)src->edgeCount * sizeof(int32_t));
    memcpy(dst->faceToHalfedgeIDs, src->faceToHalfedgeIDs,
           (size_t)src->faceCount * sizeof(int32_t));
    memcpy(dst->vertexPoints, src->vertexPoints,
           (size_t)src->vertexCount * sizeof(cc_VertexPoint));
    memcpy(dst->uvs, src->uvs, (size_t)src->uvCount * sizeof(cc_VertexUv));
    memcpy(dst->halfedges, src->halfedges,
           (size_t)src->halfedgeCount * sizeof(cc_Halfedge));
    memcpy(dst->creases, src->creases,
           (size_t)src->edgeCount * sizeof(cc_Crease));
    return dst;
}

/*
 * Build a manifold cage from face loops. Halfedges, twins, edges and the
 * vertex/edge/face mappings are derived from the loops. Boundary edges keep a
 * -1 twin. Every edge starts uncreased; UVs are one per halfedge.
 */
static cc_Mesh *
cce__BuildMesh(int32_t vertexCount, int32_t faceCount, const cce__Face *faces,
               const cc_VertexPoint *points)
{
    int32_t halfedgeCount = 0;
    for (int32_t f = 0; f < faceCount; ++f) {
        halfedgeCount += faces[f].count;
    }

    /* Pass 1: enumerate the unique undirected edges. */
    typedef struct { int32_t a, b, firstHalfedge; } cce__EdgeRec;
    cce__EdgeRec *edges = (cce__EdgeRec *)malloc((size_t)halfedgeCount * sizeof(*edges));
    int32_t edgeCount = 0;
    int32_t *halfedgeEdge = (int32_t *)malloc((size_t)halfedgeCount * sizeof(int32_t));
    int32_t *halfedgeTwin = (int32_t *)malloc((size_t)halfedgeCount * sizeof(int32_t));
    REQUIRE(edges != NULL && halfedgeEdge != NULL && halfedgeTwin != NULL);

    int32_t cursor = 0;
    for (int32_t f = 0; f < faceCount; ++f) {
        for (int32_t j = 0; j < faces[f].count; ++j) {
            const int32_t a = faces[f].vertices[j];
            const int32_t b = faces[f].vertices[(j + 1) % faces[f].count];
            int32_t edgeID = -1;
            int32_t twinID = -1;

            for (int32_t e = 0; e < edgeCount; ++e) {
                if (edges[e].a == b && edges[e].b == a) {
                    edgeID = e;
                    twinID = edges[e].firstHalfedge;
                    break;
                }
            }
            if (edgeID < 0) {
                edgeID = edgeCount++;
                edges[edgeID].a = a;
                edges[edgeID].b = b;
                edges[edgeID].firstHalfedge = cursor;
            } else {
                halfedgeTwin[edges[edgeID].firstHalfedge] = cursor;
            }
            halfedgeEdge[cursor] = edgeID;
            halfedgeTwin[cursor] = twinID;
            ++cursor;
        }
    }

    cc_Mesh *mesh = ccm_Create(vertexCount, halfedgeCount, halfedgeCount,
                               edgeCount, faceCount);
    REQUIRE(mesh != NULL);

    for (int32_t v = 0; v < vertexCount; ++v) {
        mesh->vertexPoints[v] = points[v];
        mesh->vertexToHalfedgeIDs[v] = -1;
    }

    cursor = 0;
    for (int32_t f = 0; f < faceCount; ++f) {
        mesh->faceToHalfedgeIDs[f] = cursor;
        for (int32_t j = 0; j < faces[f].count; ++j) {
            cc_Halfedge *he = &mesh->halfedges[cursor + j];
            he->vertexID = faces[f].vertices[j];
            he->faceID = f;
            he->nextID = cursor + (j + 1) % faces[f].count;
            he->prevID = cursor + (j + faces[f].count - 1) % faces[f].count;
            he->twinID = halfedgeTwin[cursor + j];
            he->edgeID = halfedgeEdge[cursor + j];
            he->uvID = cursor + j;
            mesh->vertexToHalfedgeIDs[he->vertexID] = cursor + j;
            mesh->uvs[cursor + j].u = (float)(j & 1);
            mesh->uvs[cursor + j].v = (float)((j >> 1) & 1);
        }
        cursor += faces[f].count;
    }

    for (int32_t e = 0; e < edgeCount; ++e) {
        mesh->edgeToHalfedgeIDs[e] = edges[e].firstHalfedge;
        mesh->creases[e].nextID = e;
        mesh->creases[e].prevID = e;
        mesh->creases[e].sharpness = 0.0f;
    }

    free(edges);
    free(halfedgeEdge);
    free(halfedgeTwin);
    return mesh;
}

static int32_t cce__FindEdge(const cc_Mesh *mesh, int32_t a, int32_t b)
{
    for (int32_t h = 0; h < mesh->halfedgeCount; ++h) {
        const int32_t n = mesh->halfedges[h].nextID;
        if (mesh->halfedges[h].vertexID == a &&
            mesh->halfedges[n].vertexID == b) {
            return mesh->halfedges[h].edgeID;
        }
    }
    return -1;
}

/* Quad grid with (nx * ny) quads in the z = 0 plane. */
static cc_Mesh *cce__BuildGrid(int32_t nx, int32_t ny)
{
    const int32_t vertexCount = (nx + 1) * (ny + 1);
    const int32_t faceCount = nx * ny;
    cc_VertexPoint *points = (cc_VertexPoint *)malloc(
        (size_t)vertexCount * sizeof(cc_VertexPoint));
    cce__Face *faces = (cce__Face *)malloc((size_t)faceCount * sizeof(cce__Face));
    int32_t *loops = (int32_t *)malloc((size_t)faceCount * 4 * sizeof(int32_t));
    REQUIRE(points != NULL && faces != NULL && loops != NULL);

    for (int32_t y = 0; y <= ny; ++y) {
        for (int32_t x = 0; x <= nx; ++x) {
            const int32_t v = y * (nx + 1) + x;
            points[v].x = (float)x;
            points[v].y = (float)y;
            points[v].z = 0.15f * (float)((x * 7 + y * 13) % 5);
        }
    }
    for (int32_t y = 0; y < ny; ++y) {
        for (int32_t x = 0; x < nx; ++x) {
            const int32_t f = y * nx + x;
            int32_t *loop = &loops[4 * f];
            loop[0] = y * (nx + 1) + x;
            loop[1] = y * (nx + 1) + x + 1;
            loop[2] = (y + 1) * (nx + 1) + x + 1;
            loop[3] = (y + 1) * (nx + 1) + x;
            faces[f].count = 4;
            faces[f].vertices = loop;
        }
    }

    cc_Mesh *mesh = cce__BuildMesh(vertexCount, faceCount, faces, points);
    free(points);
    free(faces);
    free(loops);
    return mesh;
}

/* Closed triangular prism: two triangles and three quads (mixed faces). */
static cc_Mesh *cce__BuildPrism(void)
{
    static const int32_t loops[5][4] = {
        {0, 2, 1, -1},       /* bottom triangle */
        {3, 4, 5, -1},       /* top triangle    */
        {0, 1, 4, 3},
        {1, 2, 5, 4},
        {2, 0, 3, 5}
    };
    static const int32_t sizes[5] = {3, 3, 4, 4, 4};
    cc_VertexPoint points[6];
    cce__Face faces[5];

    for (int32_t v = 0; v < 6; ++v) {
        points[v].x = (v < 3) ? (float)(v == 1) - (v == 2)
                              : (float)(v == 4) - (v == 5);
        points[v].y = (v == 2 || v == 5) ? 1.0f : 0.0f;
        points[v].z = (v >= 3) ? 1.0f : 0.0f;
    }
    for (int32_t f = 0; f < 5; ++f) {
        faces[f].count = sizes[f];
        faces[f].vertices = loops[f];
    }
    return cce__BuildMesh(6, 5, faces, points);
}

/* 32x32 grid plus a disconnected quad island parked far away. */
static cc_Mesh *cce__BuildGridWithIsland(int32_t gridVertices, int32_t *islandFirst)
{
    const int32_t nx = 32, ny = 32;
    const int32_t gridV = (nx + 1) * (ny + 1);
    const int32_t gridF = nx * ny;
    const int32_t vertexCount = gridV + 4;
    const int32_t faceCount = gridF + 1;
    cc_VertexPoint *points = (cc_VertexPoint *)malloc(
        (size_t)vertexCount * sizeof(cc_VertexPoint));
    cce__Face *faces = (cce__Face *)malloc((size_t)faceCount * sizeof(cce__Face));
    int32_t *loops = (int32_t *)malloc((size_t)(gridF * 4 + 4) * sizeof(int32_t));
    REQUIRE(points != NULL && faces != NULL && loops != NULL);

    for (int32_t y = 0; y <= ny; ++y) {
        for (int32_t x = 0; x <= nx; ++x) {
            const int32_t v = y * (nx + 1) + x;
            points[v].x = (float)x;
            points[v].y = (float)y;
            points[v].z = 0.0f;
        }
    }
    for (int32_t y = 0; y < ny; ++y) {
        for (int32_t x = 0; x < nx; ++x) {
            const int32_t f = y * nx + x;
            int32_t *loop = &loops[4 * f];
            loop[0] = y * (nx + 1) + x;
            loop[1] = y * (nx + 1) + x + 1;
            loop[2] = (y + 1) * (nx + 1) + x + 1;
            loop[3] = (y + 1) * (nx + 1) + x;
            faces[f].count = 4;
            faces[f].vertices = loop;
        }
    }
    *islandFirst = gridV;
    const int32_t island[4] = {gridV, gridV + 1, gridV + 2, gridV + 3};
    points[gridV + 0] = (cc_VertexPoint){1000.0f, 1000.0f, 0.0f};
    points[gridV + 1] = (cc_VertexPoint){1001.0f, 1000.0f, 0.0f};
    points[gridV + 2] = (cc_VertexPoint){1001.0f, 1001.0f, 0.0f};
    points[gridV + 3] = (cc_VertexPoint){1000.0f, 1001.0f, 0.0f};
    loops[gridF * 4 + 0] = island[0];
    loops[gridF * 4 + 1] = island[1];
    loops[gridF * 4 + 2] = island[2];
    loops[gridF * 4 + 3] = island[3];
    faces[gridF].count = 4;
    faces[gridF].vertices = &loops[gridF * 4];
    (void)gridVertices;

    cc_Mesh *mesh = cce__BuildMesh(vertexCount, faceCount, faces, points);
    free(points);
    free(faces);
    free(loops);
    return mesh;
}

/* ------------------------------------------------------------------ */
/* Reference comparison                                               */
/* ------------------------------------------------------------------ */

static int cce__PointsClose(cc_VertexPoint a, cc_VertexPoint b)
{
    for (int32_t c = 0; c < 3; ++c) {
        const float delta = fabsf(a.array[c] - b.array[c]);
        const float scale = fmaxf(1.0f, fabsf(b.array[c]));
        if (delta > 2e-5f * scale) {
            return 0;
        }
    }
    return 1;
}

/* Re-run the full Gather path on a reference cage and compare every level. */
static int
cce__CompareReference(const cc_EditPlan *plan, const cc_Mesh *reference, int32_t depth)
{
    const cc_Subd *subd = ccs_EditPlanSubd(plan);
    TASK_CHECK(subd != NULL);

    for (int32_t d = 1; d <= depth; ++d) {
        cc_Subd *ref = ccs_Create(reference, d);
        TASK_CHECK(ref != NULL);
        ccs_Refine_Gather(ref);

        const int32_t rowCount = ccs_CumulativeVertexCountAtDepth(reference, d);
        TASK_CHECK(rowCount == ccs_CumulativeVertexCountAtDepth(subd->cage, d));
        for (int32_t row = 0; row < rowCount; ++row) {
            if (!cce__PointsClose(subd->vertexPoints[row], ref->vertexPoints[row])) {
                fprintf(stderr,
                        "depth %d row %d mismatch: (%.7f %.7f %.7f) vs ref (%.7f %.7f %.7f)\n",
                        d, row,
                        subd->vertexPoints[row].x, subd->vertexPoints[row].y,
                        subd->vertexPoints[row].z,
                        ref->vertexPoints[row].x, ref->vertexPoints[row].y,
                        ref->vertexPoints[row].z);
                ccs_Release(ref);
                TASK_CHECK(0);
            }
        }
        ccs_Release(ref);
    }
    return 0;
}

/* A deterministic sequence of absolute-position batches. Ids stay within
 * 0..5 so the same sequence is valid for the smallest fixture (the prism). */
static const int32_t cce__BatchIds[4][3] = {
    {1, 4, -1},
    {4, 2, 5},
    {3, -1, -1},
    {5, -1, -1}
};
static const size_t cce__BatchSize[4] = {2, 3, 1, 1};

static void cce__ApplyBatch(cc_Mesh *mesh, int32_t batch, float lift)
{
    for (size_t i = 0; i < cce__BatchSize[batch]; ++i) {
        const int32_t id = cce__BatchIds[batch][i];
        mesh->vertexPoints[id].x += lift;
        mesh->vertexPoints[id].y -= 0.5f * lift;
        mesh->vertexPoints[id].z += 1.25f * lift;
    }
}

static int cce__RunEquivalence(cc_Mesh *cage, int32_t depth)
{
    cc_Mesh *reference = cce__DeepCopy(cage);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, depth);
    TASK_CHECK(plan != NULL);
    TASK_CHECK(ccs_EditPlanSubd(plan) != NULL);
    TASK_CHECK(cce__CompareReference(plan, reference, depth) == 0);

    for (int32_t batch = 0; batch < 4; ++batch) {
        cc_VertexPoint positions[3];
        for (size_t i = 0; i < cce__BatchSize[batch]; ++i) {
            positions[i] = cage->vertexPoints[cce__BatchIds[batch][i]];
        }
        /* last batch re-applies the current position: a no-op update */
        if (batch != 3) {
            cce__ApplyBatch(cage, batch, 0.35f);
            cce__ApplyBatch(reference, batch, 0.35f);
            for (size_t i = 0; i < cce__BatchSize[batch]; ++i) {
                positions[i] = cage->vertexPoints[cce__BatchIds[batch][i]];
            }
        } else {
            positions[0] = cage->vertexPoints[cce__BatchIds[3][0]];
        }

        cc_EditStats stats;
        const int rc = ccs_EditPlanUpdate(plan, cce__BatchIds[batch],
                                          positions, cce__BatchSize[batch], &stats);
        TASK_CHECK(rc == 0);
        TASK_CHECK(cce__CompareReference(plan, reference, depth) == 0);

        int32_t anyWork = 0;
        for (int32_t d = 0; d < depth; ++d) {
            anyWork |= (stats.evaluated_rows[d] > 0);
        }
        if (batch == 3) {
            for (int32_t d = 0; d < 5; ++d) {
                TASK_CHECK(stats.evaluated_rows[d] == 0);
                TASK_CHECK(stats.visited_rows[d] == 0);
            }
        } else {
            TASK_CHECK(anyWork);
        }
    }

    ccs_EditPlanRelease(plan);
    ccm_Release(reference);
    return 0;
}

/* ------------------------------------------------------------------ */
/* equivalence group                                                  */
/* ------------------------------------------------------------------ */

static int cce__EquivCube(void)
{
    cc_Mesh *cage = task_cube();
    TASK_CHECK(cce__RunEquivalence(cage, 5) == 0);
    ccm_Release(cage);
    return 0;
}

static int cce__EquivMixedPrism(void)
{
    cc_Mesh *cage = cce__BuildPrism();
    /* crease one vertical edge and a triangle edge to exercise semi-sharp rules */
    const int32_t e0 = cce__FindEdge(cage, 0, 1);
    const int32_t e1 = cce__FindEdge(cage, 3, 4);
    TASK_CHECK(e0 >= 0 && e1 >= 0);
    cage->creases[e0].sharpness = 2.5f;
    cage->creases[e1].sharpness = 1.0f;
    TASK_CHECK(cce__RunEquivalence(cage, 4) == 0);
    ccm_Release(cage);
    return 0;
}

static int cce__EquivOpenCreasedGrid(void)
{
    cc_Mesh *cage = cce__BuildGrid(4, 3);
    /* open boundary plus interior and boundary semi-sharp creases */
    const int32_t interior = cce__FindEdge(cage, 6, 7);
    const int32_t boundary = cce__FindEdge(cage, 0, 1);
    const int32_t hard = cce__FindEdge(cage, 10, 11);
    TASK_CHECK(interior >= 0 && boundary >= 0 && hard >= 0);
    cage->creases[interior].sharpness = 1.75f;
    cage->creases[boundary].sharpness = 0.5f;
    cage->creases[hard].sharpness = 4.0f;
    TASK_CHECK(cce__RunEquivalence(cage, 5) == 0);
    ccm_Release(cage);
    return 0;
}

/*
 * Editing positions must leave the subdivided topology, UVs and creases
 * untouched: only the vertex-point buffer is ever rewritten.
 */
static int cce__EquivFixedTopologyUvCrease(void)
{
    cc_Mesh *cage = cce__BuildGrid(3, 3);
    const int32_t creased = cce__FindEdge(cage, 4, 5);
    TASK_CHECK(creased >= 0);
    cage->creases[creased].sharpness = 2.25f;

    const int32_t depth = 3;
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, depth);
    TASK_CHECK(plan != NULL);
    const cc_Subd *subd = ccs_EditPlanSubd(plan);

    /* A fresh full refinement gives the reference topology/UV/crease buffers. */
    cc_Subd *refSubd = ccs_Create(cage, depth);
    TASK_CHECK(refSubd != NULL);
    ccs_Refine_Gather(refSubd);

    const int32_t halfedgeTotal = ccs_CumulativeHalfedgeCountAtDepth(cage, depth);
    const int32_t creaseTotal = ccs_CumulativeCreaseCountAtDepth(cage, depth);
    const size_t heBytes = (size_t)halfedgeTotal * sizeof(cc_Halfedge_SemiRegular);
    const size_t crBytes = (size_t)creaseTotal * sizeof(cc_Crease);
    TASK_CHECK(memcmp(subd->halfedges, refSubd->halfedges, heBytes) == 0);
    TASK_CHECK(memcmp(subd->creases, refSubd->creases, crBytes) == 0);

    cc_Halfedge_SemiRegular *heBefore = (cc_Halfedge_SemiRegular *)malloc(heBytes);
    cc_Crease *crBefore = (cc_Crease *)malloc(crBytes);
    TASK_CHECK(heBefore != NULL && crBefore != NULL);
    memcpy(heBefore, subd->halfedges, heBytes);
    memcpy(crBefore, subd->creases, crBytes);

    /* a multi-vertex drag at an interior vertex */
    const int32_t ids[2] = {5, 6};
    cc_VertexPoint pos[2];
    pos[0] = cage->vertexPoints[5]; pos[0].z += 1.5f;
    pos[1] = cage->vertexPoints[6]; pos[1].x -= 0.7f;
    cc_EditStats stats;
    TASK_CHECK(ccs_EditPlanUpdate(plan, ids, pos, 2, &stats) == 0);
    TASK_CHECK(stats.evaluated_rows[0] > 0);

    /* topology, halfedge-encoded UVs and crease rules survive the edit */
    TASK_CHECK(memcmp(subd->halfedges, heBefore, heBytes) == 0);
    TASK_CHECK(memcmp(subd->creases, crBefore, crBytes) == 0);
    TASK_CHECK(memcmp(subd->halfedges, refSubd->halfedges, heBytes) == 0);
    TASK_CHECK(memcmp(subd->creases, refSubd->creases, crBytes) == 0);

    free(heBefore);
    free(crBefore);
    ccs_Release(refSubd);
    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    return 0;
}

/* ------------------------------------------------------------------ */
/* locality group                                                     */
/* ------------------------------------------------------------------ */

static int cce__LocalityFootprint(void)
{
    int32_t islandFirst = 0;
    cc_Mesh *cage = cce__BuildGridWithIsland(0, &islandFirst);
    const int32_t totalRows = ccs_CumulativeVertexCountAtDepth(cage, 2);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 2);
    TASK_CHECK(plan != NULL);
    const cc_Subd *subd = ccs_EditPlanSubd(plan);

    /* snapshot the island cage rows and every far-away row */
    const size_t byteCount = (size_t)totalRows * sizeof(cc_VertexPoint);
    cc_VertexPoint *before = (cc_VertexPoint *)malloc(byteCount);
    TASK_CHECK(before != NULL);
    memcpy(before, subd->vertexPoints, byteCount);

    /* drag a single interior grid vertex at depth 2 */
    const int32_t dragId = 16 * 33 + 16;
    cc_VertexPoint dragPos = cage->vertexPoints[dragId];
    dragPos.z += 2.0f;
    cc_EditStats stats;
    TASK_CHECK(ccs_EditPlanUpdate(plan, &dragId, &dragPos, 1, &stats) == 0);

    TASK_CHECK(stats.evaluated_rows[1] > 0);
    TASK_CHECK(stats.visited_rows[1] > 0);
    TASK_CHECK(stats.visited_rows[1] < (size_t)totalRows / 4);
    TASK_CHECK(stats.evaluated_rows[1] <= stats.visited_rows[1]);

    /* the disconnected island and any row parked far away keep their bytes */
    for (int32_t v = islandFirst; v < cage->vertexCount; ++v) {
        TASK_CHECK(memcmp(&subd->vertexPoints[v], &before[v],
                          sizeof(cc_VertexPoint)) == 0);
    }
    for (int32_t row = 0; row < totalRows; ++row) {
        if (before[row].x > 100.0f) {
            TASK_CHECK(memcmp(&subd->vertexPoints[row], &before[row],
                              sizeof(cc_VertexPoint)) == 0);
        }
    }

    /* the dragged region really did move */
    TASK_CHECK(memcmp(&subd->vertexPoints[dragId], &before[dragId],
                      sizeof(cc_VertexPoint)) != 0);

    free(before);
    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    return 0;
}

static int cce__LocalityNoChange(void)
{
    cc_Mesh *cage = cce__BuildGrid(8, 8);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 3);
    TASK_CHECK(plan != NULL);
    const cc_Subd *subd = ccs_EditPlanSubd(plan);
    const int32_t totalRows = ccs_CumulativeVertexCountAtDepth(cage, 3);
    const size_t byteCount = (size_t)totalRows * sizeof(cc_VertexPoint);
    cc_VertexPoint *before = (cc_VertexPoint *)malloc(byteCount);
    TASK_CHECK(before != NULL);
    memcpy(before, subd->vertexPoints, byteCount);

    /* empty batch with NULL arrays succeeds and does nothing */
    cc_EditStats stats;
    TASK_CHECK(ccs_EditPlanUpdate(plan, NULL, NULL, 0, &stats) == 0);
    for (int32_t d = 0; d < 5; ++d) {
        TASK_CHECK(stats.evaluated_rows[d] == 0);
        TASK_CHECK(stats.visited_rows[d] == 0);
    }

    /* writing the same absolute positions produces no geometry work */
    const int32_t ids[2] = {7, 42};
    cc_VertexPoint positions[2];
    positions[0] = cage->vertexPoints[7];
    positions[1] = cage->vertexPoints[42];
    TASK_CHECK(ccs_EditPlanUpdate(plan, ids, positions, 2, &stats) == 0);
    for (int32_t d = 0; d < 5; ++d) {
        TASK_CHECK(stats.evaluated_rows[d] == 0);
        TASK_CHECK(stats.visited_rows[d] == 0);
    }
    TASK_CHECK(memcmp(subd->vertexPoints, before, byteCount) == 0);

    free(before);
    ccs_EditPlanRelease(plan);
    ccm_Release(cage);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ownership group                                                    */
/* ------------------------------------------------------------------ */

static int cce__OwnSourceRelease(void)
{
    cc_Mesh *cage = task_cube();
    cc_Mesh *reference = cce__DeepCopy(cage);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 3);
    TASK_CHECK(plan != NULL);
    const cc_Subd *subd = ccs_EditPlanSubd(plan);
    TASK_CHECK(subd->cage != cage);

    /* release the source cage: the plan keeps working on its private copy */
    ccm_Release(cage);

    cc_VertexPoint pos = subd->cage->vertexPoints[2];
    pos.x += 0.7f;
    const int32_t id = 2;
    cc_EditStats stats;
    TASK_CHECK(ccs_EditPlanUpdate(plan, &id, &pos, 1, &stats) == 0);
    reference->vertexPoints[2] = pos;
    TASK_CHECK(cce__CompareReference(plan, reference, 3) == 0);

    ccs_EditPlanRelease(plan);
    ccm_Release(reference);
    return 0;
}

static int cce__OwnDualPlans(void)
{
    cc_Mesh *cage = cce__BuildGrid(5, 5);
    cc_Mesh *refA = cce__DeepCopy(cage);
    cc_Mesh *refB = cce__DeepCopy(cage);
    cc_EditPlan *planA = ccs_EditPlanCreate(cage, 3);
    cc_EditPlan *planB = ccs_EditPlanCreate(cage, 2);
    TASK_CHECK(planA != NULL && planB != NULL);
    TASK_CHECK(ccs_EditPlanSubd(planA) != ccs_EditPlanSubd(planB));

    /* interleave conflicting drags; each plan must track its own state */
    for (int32_t step = 0; step < 4; ++step) {
        const int32_t idA = 3 + step;
        const int32_t idB = 20 - step;
        cc_VertexPoint posA = ccs_EditPlanSubd(planA)->cage->vertexPoints[idA];
        cc_VertexPoint posB = ccs_EditPlanSubd(planB)->cage->vertexPoints[idB];
        posA.z += 0.4f;
        posB.x -= 0.6f;

        TASK_CHECK(ccs_EditPlanUpdate(planA, &idA, &posA, 1, NULL) == 0);
        TASK_CHECK(ccs_EditPlanUpdate(planB, &idB, &posB, 1, NULL) == 0);
        refA->vertexPoints[idA] = posA;
        refB->vertexPoints[idB] = posB;

        TASK_CHECK(cce__CompareReference(planA, refA, 3) == 0);
        TASK_CHECK(cce__CompareReference(planB, refB, 2) == 0);
    }

    ccs_EditPlanRelease(planA);
    ccs_EditPlanRelease(planB);
    ccm_Release(refA);
    ccm_Release(refB);
    ccm_Release(cage);
    return 0;
}

static int cce__OwnFailureRecovery(void)
{
    cc_Mesh *cage = cce__BuildGrid(6, 6);
    cc_Mesh *reference = cce__DeepCopy(cage);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 3);
    TASK_CHECK(plan != NULL);
    const cc_Subd *subd = ccs_EditPlanSubd(plan);
    const int32_t totalRows = ccs_CumulativeVertexCountAtDepth(cage, 3);
    const size_t byteCount = (size_t)totalRows * sizeof(cc_VertexPoint);
    const size_t cageBytes = (size_t)cage->vertexCount * sizeof(cc_VertexPoint);
    cc_VertexPoint *snapshot = (cc_VertexPoint *)malloc(byteCount);
    cc_VertexPoint *cageSnapshot = (cc_VertexPoint *)malloc(cageBytes);
    TASK_CHECK(snapshot != NULL && cageSnapshot != NULL);
    memcpy(snapshot, subd->vertexPoints, byteCount);

    /* a valid edit first */
    cc_VertexPoint good = cage->vertexPoints[10];
    good.z += 1.0f;
    const int32_t goodId = 10;
    TASK_CHECK(ccs_EditPlanUpdate(plan, &goodId, &good, 1, NULL) == 0);
    reference->vertexPoints[10] = good;
    memcpy(snapshot, subd->vertexPoints, byteCount);
    memcpy(cageSnapshot, subd->cage->vertexPoints, cageBytes);

    /* duplicated indices are rejected */
    const int32_t dupIds[2] = {4, 4};
    cc_VertexPoint dupPos[2] = {cage->vertexPoints[4], cage->vertexPoints[4]};
    dupPos[0].x += 1.0f;
    TASK_CHECK(ccs_EditPlanUpdate(plan, dupIds, dupPos, 2, NULL) != 0);
    TASK_CHECK(memcmp(subd->vertexPoints, snapshot, byteCount) == 0);
    TASK_CHECK(memcmp(subd->cage->vertexPoints, cageSnapshot, cageBytes) == 0);

    /* out of range id rejected */
    const int32_t badId = cage->vertexCount + 5;
    cc_VertexPoint badPos = cage->vertexPoints[0];
    TASK_CHECK(ccs_EditPlanUpdate(plan, &badId, &badPos, 1, NULL) != 0);
    TASK_CHECK(memcmp(subd->vertexPoints, snapshot, byteCount) == 0);
    TASK_CHECK(memcmp(subd->cage->vertexPoints, cageSnapshot, cageBytes) == 0);

    /* non-finite position rejected */
    const int32_t nanId = 5;
    cc_VertexPoint nanPos = cage->vertexPoints[5];
    nanPos.y = NAN;
    TASK_CHECK(ccs_EditPlanUpdate(plan, &nanId, &nanPos, 1, NULL) != 0);
    TASK_CHECK(memcmp(subd->vertexPoints, snapshot, byteCount) == 0);
    TASK_CHECK(memcmp(subd->cage->vertexPoints, cageSnapshot, cageBytes) == 0);

    /* non-empty batch with NULL arrays rejected */
    TASK_CHECK(ccs_EditPlanUpdate(plan, NULL, NULL, 1, NULL) != 0);
    TASK_CHECK(memcmp(subd->vertexPoints, snapshot, byteCount) == 0);
    TASK_CHECK(memcmp(subd->cage->vertexPoints, cageSnapshot, cageBytes) == 0);

    /* the plan still works and accumulates on the last successful state */
    cc_VertexPoint next = cage->vertexPoints[11];
    next.x -= 0.8f;
    const int32_t nextId = 11;
    cc_EditStats stats;
    TASK_CHECK(ccs_EditPlanUpdate(plan, &nextId, &next, 1, &stats) == 0);
    TASK_CHECK(stats.evaluated_rows[0] > 0);
    reference->vertexPoints[11] = next;
    TASK_CHECK(cce__CompareReference(plan, reference, 3) == 0);

    free(snapshot);
    free(cageSnapshot);
    ccs_EditPlanRelease(plan);
    ccm_Release(reference);
    ccm_Release(cage);
    return 0;
}

static int cce__OwnInvalidArgs(void)
{
    cc_Mesh *cage = task_cube();

    TASK_CHECK(ccs_EditPlanCreate(NULL, 2) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, 0) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, 6) == NULL);
    TASK_CHECK(ccs_EditPlanCreate(cage, -1) == NULL);
    ccs_EditPlanRelease(NULL); /* must not crash */
    TASK_CHECK(ccs_EditPlanSubd(NULL) == NULL);
    TASK_CHECK(ccs_EditPlanUpdate(NULL, NULL, NULL, 0, NULL) != 0);

    /* allocation failure: every partial construction must be cleaned up */
    cce_test_fail_after = 0;
    long injectedFailures = 0;
    while (injectedFailures < 256) {
        cce_test_fail_after = injectedFailures;
        cc_EditPlan *plan = ccs_EditPlanCreate(cage, 2);
        if (plan != NULL) {
            ccs_EditPlanRelease(plan);
            break; /* enough failures exercised: construction eventually works */
        }
        TASK_CHECK(cce_test_live == 0); /* nothing leaked from the failed create */
        injectedFailures += 1;
    }
    cce_test_fail_after = -1;
    TASK_CHECK(injectedFailures > 0);

    /* a normally allocated plan still balances the tracked allocations */
    {
        cc_EditPlan *plan = ccs_EditPlanCreate(cage, 1);
        TASK_CHECK(plan != NULL);
        ccs_EditPlanRelease(plan);
        TASK_CHECK(cce_test_live == 0);
    }

    ccm_Release(cage);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test registry                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"equivalence", "cube_all_depths",            cce__EquivCube},
    {"equivalence", "closed_mixed_faces_creases", cce__EquivMixedPrism},
    {"equivalence", "open_boundary_semisharp",    cce__EquivOpenCreasedGrid},
    {"equivalence", "topology_uv_crease_fixed",   cce__EquivFixedTopologyUvCrease},

    {"locality", "single_drag_footprint",         cce__LocalityFootprint},
    {"locality", "no_change_zero_stats",          cce__LocalityNoChange},

    {"ownership", "source_released",              cce__OwnSourceRelease},
    {"ownership", "dual_plans_interleaved",       cce__OwnDualPlans},
    {"ownership", "rejected_batch_recovery",      cce__OwnFailureRecovery},
    {"ownership", "invalid_args_and_alloc_fail",  cce__OwnInvalidArgs},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
