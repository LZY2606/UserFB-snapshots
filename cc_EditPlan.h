/*
 * cc_EditPlan.h -- sparse, reusable Catmull-Clark edit plan.
 *
 * An cc_EditPlan owns a private copy of a control cage together with a fully
 * subdivided cc_Subd. Cage edits are applied through ccs_EditPlanUpdate, which
 * propagates position changes along the exact face/edge/vertex dependencies of
 * the (fixed) topology and crease rules, instead of recomputing every level.
 *
 * Usage:
 *   #define CC_EDIT_IMPLEMENTATION
 *   #include "cc_EditPlan.h"
 *
 * The plan never mutates the source cage: it deep-copies the cage (points,
 * UVs, halfedges, mappings and creases) at construction time, so the original
 * mesh may be edited or released afterwards. Topology, UVs and creases are
 * fixed for the lifetime of a plan; only positions ever change.
 */
#ifndef CC_EDIT_PLAN_H
#define CC_EDIT_PLAN_H

#include "CatmullClark.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-update workload counters, indexed by output depth (entry 0 == depth 1). */
typedef struct {
    size_t evaluated_rows[5]; /* position rows actually (re-)evaluated       */
    size_t visited_rows[5];   /* candidate position rows examined            */
} cc_EditStats;

/* Opaque edit plan. */
typedef struct cc_EditPlan cc_EditPlan;

/*
 * Create a plan for a valid manifold cage, subdivided down to `depth` levels
 * (1..5). The cage, its UVs, halfedges, mappings and creases are copied and
 * the full initial subdivision is performed. Returns NULL on a NULL mesh, an
 * illegal depth, or allocation failure (all allocated resources are freed).
 */
cc_EditPlan *ccs_EditPlanCreate(const cc_Mesh *cage, int32_t depth);

/* Release a plan and every resource it owns. NULL is accepted. */
void ccs_EditPlanRelease(cc_EditPlan *plan);

/* Read-only access to the plan-owned subdivision result. */
const cc_Subd *ccs_EditPlanSubd(const cc_EditPlan *plan);

/*
 * Apply a batch of absolute cage vertex positions.
 *
 * Returns 0 on success, non-zero on failure. The batch is rejected (leaving
 * every stored position untouched) when an id is duplicated or out of range,
 * a position is non-finite, or a non-empty batch carries NULL arrays. An empty
 * batch (count == 0, arrays may be NULL) succeeds and does no work. On success
 * stats is fully overwritten; creation-time preprocessing is never counted.
 */
int ccs_EditPlanUpdate(cc_EditPlan *plan,
                       const int32_t *ids,
                       const cc_VertexPoint *positions,
                       size_t count,
                       cc_EditStats *stats);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CC_EDIT_PLAN_H

#ifdef CC_EDIT_IMPLEMENTATION

#ifndef CCE_ASSERT
#    include <assert.h>
#    define CCE_ASSERT(x) assert(x)
#endif

#ifndef CCE_MALLOC
#    include <stdlib.h>
#    define CCE_MALLOC(x) malloc(x)
#    define CCE_FREE(x)   free(x)
#else
#    ifndef CCE_FREE
#        error CCE_MALLOC defined without CCE_FREE
#    endif
#endif

#ifndef CCE_MEMCPY
#    include <string.h>
#    define CCE_MEMCPY(dest, src, count) memcpy(dest, src, count)
#endif

#ifndef CCE_MEMSET
#    include <string.h>
#    define CCE_MEMSET(ptr, value, num) memset(ptr, value, num)
#endif

#include <math.h>

#define CCE_MAX_DEPTH 5

/* ------------------------------------------------------------------ */
/* Per-level dependency cache                                         */
/* ------------------------------------------------------------------ */

/*
 * Dependency cache for one subdivision step.
 *
 * Level 0 is the cage step (cage -> depth 1); level L >= 1 refines the quad
 * mesh at subd depth L into depth L + 1 (mirroring the upstream loop
 * `for (depth = 1; depth < maxDepth; ++depth)`).
 *
 * Row layout (all indices are global indices into cc_Subd::vertexPoints):
 *   read range  : [readOffset,  readOffset + vertexCount)
 *                 old input vertices; for level L > 0 this is exactly the
 *                 output block written by level L - 1.
 *   output block: [writeOffset, writeOffset + vertexCount + faceCount + edgeCount)
 *                 holding new vertex, face and edge point rows in that order.
 *
 * Forward dependencies ("position dependencies") store, for every output row,
 * the global rows its rule reads. The reverse index maps every readable row to
 * the output rows that depend on it, so an edit propagates without scanning
 * the level.
 */
typedef struct {
    int32_t vertexCount;
    int32_t edgeCount;
    int32_t faceCount;
    int32_t creaseCount;

    int32_t readOffset;
    int32_t writeOffset;
    int32_t faceOffset; /* global index of the first face point */
    int32_t edgeOffset; /* global index of the first edge point */
    int32_t blockEnd;   /* one past the last output row          */
    int32_t halfedgeStride;
    int32_t creaseStride;

    /* Forward dependencies, packed per rule in vertex/face/edge order. */
    int32_t *faceRefs;
    int32_t *edgeRefs;
    int32_t *vertexRefs;
    int32_t *faceRefCount;
    int32_t *edgeRefCount;
    int32_t *vertexRefCount;
    size_t faceRefCapacity;
    size_t edgeRefCapacity;
    size_t vertexRefCapacity;

    /* Reverse index (CSR), indexed by (global id - readOffset). */
    int32_t reverseSpan;
    int32_t *reverseOffset;
    int32_t *reverseAdj;
} cce__Level;

struct cc_EditPlan {
    cc_Mesh *cage;
    cc_Subd *subd;
    int32_t depth;

    int32_t cageVertexCount;
    int32_t totalVertexCount;

    cce__Level levels[CCE_MAX_DEPTH];

    int32_t *marks;          /* footprint dedup, indexed by global vertex id */
    int32_t markGeneration;  /* single counter shared by every level         */
    int32_t *dupMarks;      /* batch duplicate detection, indexed by cage id */
    int32_t dupGeneration;

    int32_t *dirty;         /* growing work list, reused for every level     */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static float cce__Satf(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float cce__Signf(float x)
{
    if (x < 0.0f) {
        return -1.0f;
    } else if (x > 0.0f) {
        return 1.0f;
    }
    return 0.0f;
}

static int32_t cce__MaxInt(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

/* Halfedge topology accessors, shared by cache construction and evaluation. */
static int32_t cce__TwinID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].twinID;
    }
    return plan->subd->halfedges[plan->levels[level].halfedgeStride + h].twinID;
}

static int32_t cce__NextID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].nextID;
    }
    return ccm_HalfedgeNextID_Quad(h);
}

static int32_t cce__PrevID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].prevID;
    }
    return ccm_HalfedgePrevID_Quad(h);
}

static int32_t cce__EdgeID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].edgeID;
    }
    return plan->subd->halfedges[plan->levels[level].halfedgeStride + h].edgeID;
}

static int32_t cce__FaceID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].faceID;
    }
    return ccm_HalfedgeFaceID_Quad(h);
}

static int32_t cce__VertexID(const cc_EditPlan *plan, int32_t level, int32_t h)
{
    if (level == 0) {
        return plan->cage->halfedges[h].vertexID;
    }
    return plan->subd->halfedges[plan->levels[level].halfedgeStride + h].vertexID;
}

static float cce__EdgeSharpness(const cc_EditPlan *plan, int32_t level, int32_t edgeID)
{
    const cce__Level *lv = &plan->levels[level];

    if (level == 0) {
        return plan->cage->creases[edgeID].sharpness;
    }
    if (edgeID >= lv->creaseCount) {
        return 0.0f; /* edges created inside a face are never sharp */
    }
    return plan->subd->creases[lv->creaseStride + edgeID].sharpness;
}

/*
 * Vertex -> halfedge mapping at a subd depth, ported from the upstream
 * (non-recursive) ccs_VertexPointToHalfedgeID. The public header declares no
 * prototype for it, so the plan carries its own copy of the fixed mapping.
 */
static int32_t cce__VertexToHalfedgeFirst(const cc_Mesh *cage, int32_t vertexID)
{
    const int32_t vertexCount = ccm_VertexCount(cage);
    const int32_t faceCount = ccm_FaceCount(cage);

    if (vertexID >= vertexCount + faceCount) {
        const int32_t edgeID = vertexID - vertexCount - faceCount;
        return 4 * ccm_EdgeToHalfedgeID(cage, edgeID) + 1;
    } else if (vertexID >= vertexCount) {
        const int32_t faceID = vertexID - vertexCount;
        return 4 * ccm_FaceToHalfedgeID(cage, faceID) + 2;
    }
    return 4 * ccm_VertexToHalfedgeID(cage, vertexID) + 0;
}

static int32_t
cce__VertexToHalfedgeID(const cc_Subd *subd, int32_t vertexID, int32_t depth)
{
    const cc_Mesh *cage = subd->cage;
    int32_t heapDepth = depth;
    int32_t stride = 0;
    int32_t halfedgeID;

    for (; heapDepth > 1; --heapDepth) {
        const int32_t vertexCount = ccm_VertexCountAtDepth_Fast(cage, heapDepth - 1);
        const int32_t faceCount = ccm_FaceCountAtDepth_Fast(cage, heapDepth - 1);

        if (vertexID >= vertexCount + faceCount) {
            const int32_t edgeID = vertexID - faceCount - vertexCount;
            halfedgeID = 4 * ccs_EdgeToHalfedgeID(subd, edgeID, heapDepth - 1) + 1;
            break;
        } else if (vertexID >= vertexCount) {
            const int32_t faceID = vertexID - vertexCount;
            halfedgeID = 4 * ccm_FaceToHalfedgeID_Quad(faceID) + 2;
            break;
        } else {
            stride += 2;
        }
    }

    if (heapDepth == 1) {
        halfedgeID = cce__VertexToHalfedgeFirst(cage, vertexID);
    }

    return halfedgeID << stride;
}

/*
 * Position reads. At level 0 the "old vertex" inputs are the raw cage control
 * points (the subd buffer's first V slots instead hold the smoothed depth-1
 * vertices, exactly as upstream overwrites them in ccs__CageVertexPoints_*).
 * Face and edge point rows, and every row at deeper levels, live in the subd.
 */
static const cc_VertexPoint *
cce__PointAt(const cc_EditPlan *plan, int32_t level, int32_t globalID)
{
    if (level == 0 && globalID < plan->cageVertexCount) {
        return &plan->cage->vertexPoints[globalID];
    }
    return &plan->subd->vertexPoints[globalID];
}

/*
 * Evaluated rows are always subd positions. The cage keeps only raw control
 * points, which are written exclusively by ccs_EditPlanUpdate.
 */
static void
cce__WritePoint(cc_EditPlan *plan, int32_t level, int32_t globalID, cc_VertexPoint p)
{
    (void)level;
    plan->subd->vertexPoints[globalID] = p;
}

/* ------------------------------------------------------------------ */
/* Dynamic int array                                                  */
/* ------------------------------------------------------------------ */

static int cce__GrowInts(int32_t **buffer, size_t *capacity, size_t required)
{
    size_t newCapacity = *capacity ? *capacity : 16;
    int32_t *newBuffer;

    while (newCapacity < required) {
        newCapacity *= 2;
    }
    newBuffer = (int32_t *)CCE_MALLOC(newCapacity * sizeof(int32_t));
    if (newBuffer == NULL) {
        return 0;
    }
    if (*buffer != NULL) {
        CCE_MEMCPY(newBuffer, *buffer, (*capacity) * sizeof(int32_t));
        CCE_FREE(*buffer);
    }
    *buffer = newBuffer;
    *capacity = newCapacity;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Forward dependency collection (exact mirrors of the Gather walks)  */
/* ------------------------------------------------------------------ */

/*
 * Append a reference to one rule's own ref slice, which starts at sliceStart in
 * the packed buffer. Deduplication is local to that rule, so a row shared by
 * several rules stays listed in every one of them.
 */
static int cce__AppendRef(int32_t globalID, size_t sliceStart,
                          int32_t **out, size_t *outCount, size_t *outCapacity)
{
    for (size_t i = sliceStart; i < *outCount; ++i) {
        if ((*out)[i] == globalID) {
            return 1;
        }
    }
    if (*outCount >= *outCapacity &&
        !cce__GrowInts(out, outCapacity, *outCount + 1)) {
        return 0;
    }
    (*out)[(*outCount)++] = globalID;

    return 1;
}

/* Face rule: every corner vertex of the face. */
static int
cce__BuildFaceRefs(cc_EditPlan *plan, int32_t level, int32_t faceID,
                   int32_t **out, size_t sliceStart,
                   size_t *outCount, size_t *outCapacity)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t first = level == 0
        ? plan->cage->faceToHalfedgeIDs[faceID]
        : ccm_FaceToHalfedgeID_Quad(faceID);
    int32_t h = first;

    do {
        const int32_t globalID = lv->readOffset + cce__VertexID(plan, level, h);
        if (!cce__AppendRef(globalID, sliceStart, out, outCount, outCapacity)) {
            return 0;
        }
        h = cce__NextID(plan, level, h);
    } while (h != first);

    return 1;
}

/* Edge rule: the two endpoints and the two adjacent face points. */
static int
cce__BuildEdgeRefs(cc_EditPlan *plan, int32_t level, int32_t edgeID,
                   int32_t **out, size_t sliceStart,
                   size_t *outCount, size_t *outCapacity)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t h = level == 0
        ? plan->cage->edgeToHalfedgeIDs[edgeID]
        : ccs_EdgeToHalfedgeID(plan->subd, edgeID, level);
    const int32_t twinID = cce__TwinID(plan, level, h);
    const int32_t nextID = cce__NextID(plan, level, h);
    const int32_t ids[4] = {
        lv->readOffset + cce__VertexID(plan, level, h),
        lv->readOffset + cce__VertexID(plan, level, nextID),
        lv->faceOffset + cce__FaceID(plan, level, h),
        lv->faceOffset + cce__FaceID(plan, level, cce__MaxInt(0, twinID))
    };

    for (int i = 0; i < 4; ++i) {
        if (!cce__AppendRef(ids[i], sliceStart, out, outCount, outCapacity)) {
            return 0;
        }
    }

    return 1;
}

/*
 * Vertex rule dependency walk. This enumerates exactly the rows read by
 * ccs__CreasedCageVertexPoints_Gather (level 0) and
 * ccs__CreasedVertexPoints_Gather (level > 0).
 */
static int
cce__BuildVertexRefs(cc_EditPlan *plan, int32_t level, int32_t vertexID,
                     int32_t **out, size_t sliceStart,
                     size_t *outCount, size_t *outCapacity)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t h = level == 0
        ? plan->cage->vertexToHalfedgeIDs[vertexID]
        : cce__VertexToHalfedgeID(plan->subd, vertexID, level);
    const int32_t edgeID = cce__EdgeID(plan, level, h);
    const int32_t prevID = cce__PrevID(plan, level, h);
    const int32_t prevEdgeID = cce__EdgeID(plan, level, prevID);
    const int32_t prevFaceID = cce__FaceID(plan, level, prevID);
    int32_t iterator;

    /* seed contributions: E(edge(h)), F(face(prev(h))), E(edge(prev(h))) */
    {
        const int32_t ids[3] = {
            lv->edgeOffset + edgeID,
            lv->faceOffset + prevFaceID,
            lv->edgeOffset + prevEdgeID
        };
        for (int i = 0; i < 3; ++i) {
            if (!cce__AppendRef(ids[i], sliceStart, out, outCount, outCapacity)) {
                return 0;
            }
        }
    }

    /* backward ring around the vertex */
    for (iterator = cce__TwinID(plan, level, prevID);
         iterator >= 0 && iterator != h;
         iterator = cce__TwinID(plan, level, iterator)) {
        const int32_t ringPrev = cce__PrevID(plan, level, iterator);
        const int32_t ids[2] = {
            lv->edgeOffset + cce__EdgeID(plan, level, ringPrev),
            lv->faceOffset + cce__FaceID(plan, level, ringPrev)
        };
        for (int i = 0; i < 2; ++i) {
            if (!cce__AppendRef(ids[i], sliceStart, out, outCount, outCapacity)) {
                return 0;
            }
        }
        iterator = ringPrev;
    }

    /* the subd gather also walks the forward ring of a boundary vertex */
    if (level > 0) {
        int32_t backward = cce__TwinID(plan, level, h);
        while (iterator < 0 && backward >= 0 && backward != h) {
            const int32_t ringNext = cce__NextID(plan, level, backward);
            const int32_t ids[2] = {
                lv->edgeOffset + cce__EdgeID(plan, level, ringNext),
                lv->faceOffset + cce__FaceID(plan, level, ringNext)
            };
            for (int i = 0; i < 2; ++i) {
                if (!cce__AppendRef(ids[i], sliceStart, out, outCount, outCapacity)) {
                    return 0;
                }
            }
            backward = cce__TwinID(plan, level, ringNext);
        }
    }

    /* boundary correction: the seed edge point is read again */
    if (iterator < 0) {
        if (!cce__AppendRef(lv->edgeOffset + edgeID, sliceStart,
                            out, outCount, outCapacity)) {
            return 0;
        }
    }

    /* the old position of the vertex itself */
    return cce__AppendRef(lv->readOffset + vertexID, sliceStart,
                          out, outCount, outCapacity);
}

static int cce__BuildLevelCache(cc_EditPlan *plan, int32_t level)
{
    cce__Level *lv = &plan->levels[level];

    lv->faceRefCount = (int32_t *)CCE_MALLOC((size_t)lv->faceCount * sizeof(int32_t));
    lv->edgeRefCount = (int32_t *)CCE_MALLOC((size_t)lv->edgeCount * sizeof(int32_t));
    lv->vertexRefCount = (int32_t *)CCE_MALLOC((size_t)lv->vertexCount * sizeof(int32_t));
    if (lv->faceRefCount == NULL || lv->edgeRefCount == NULL ||
        lv->vertexRefCount == NULL) {
        return 0;
    }

    /*
     * Refs are packed back-to-back: a running cursor is handed to each builder
     * and the per-rule count is the slice it added. Resetting the cursor per
     * rule would make every rule overwrite the start of the shared buffer.
     */
    size_t faceTotal = 0, facePrev = 0;
    for (int32_t faceID = 0; faceID < lv->faceCount; ++faceID) {
        facePrev = faceTotal;
        if (!cce__BuildFaceRefs(plan, level, faceID,
                                &lv->faceRefs, facePrev,
                                &faceTotal, &lv->faceRefCapacity)) {
            return 0;
        }
        lv->faceRefCount[faceID] = (int32_t)(faceTotal - facePrev);
    }

    size_t edgeTotal = 0, edgePrev = 0;
    for (int32_t edgeID = 0; edgeID < lv->edgeCount; ++edgeID) {
        edgePrev = edgeTotal;
        if (!cce__BuildEdgeRefs(plan, level, edgeID,
                                &lv->edgeRefs, edgePrev,
                                &edgeTotal, &lv->edgeRefCapacity)) {
            return 0;
        }
        lv->edgeRefCount[edgeID] = (int32_t)(edgeTotal - edgePrev);
    }

    size_t vertexTotal = 0, vertexPrev = 0;
    for (int32_t vertexID = 0; vertexID < lv->vertexCount; ++vertexID) {
        vertexPrev = vertexTotal;
        if (!cce__BuildVertexRefs(plan, level, vertexID,
                                  &lv->vertexRefs, vertexPrev,
                                  &vertexTotal, &lv->vertexRefCapacity)) {
            return 0;
        }
        lv->vertexRefCount[vertexID] = (int32_t)(vertexTotal - vertexPrev);
    }

    return 1;
}

/* Build the CSR reverse index from the packed forward dependencies. */
static int cce__BuildReverseIndex(cc_EditPlan *plan, int32_t level)
{
    cce__Level *lv = &plan->levels[level];
    const int32_t span = lv->blockEnd - lv->readOffset;
    size_t totalRefs = 0;

    lv->reverseSpan = span;
    lv->reverseOffset = (int32_t *)CCE_MALLOC((size_t)(span + 1) * sizeof(int32_t));
    if (lv->reverseOffset == NULL) {
        return 0;
    }
    CCE_MEMSET(lv->reverseOffset, 0, (size_t)(span + 1) * sizeof(int32_t));

    for (int32_t i = 0; i < lv->faceCount; ++i) {
        totalRefs += (size_t)lv->faceRefCount[i];
    }
    for (int32_t i = 0; i < lv->edgeCount; ++i) {
        totalRefs += (size_t)lv->edgeRefCount[i];
    }
    for (int32_t i = 0; i < lv->vertexCount; ++i) {
        totalRefs += (size_t)lv->vertexRefCount[i];
    }

    /* degree pass */
    int32_t faceCursor = 0;
    for (int32_t faceID = 0; faceID < lv->faceCount; ++faceID) {
        for (int32_t r = 0; r < lv->faceRefCount[faceID]; ++r) {
            lv->reverseOffset[lv->faceRefs[faceCursor + r] - lv->readOffset + 1] += 1;
        }
        faceCursor += lv->faceRefCount[faceID];
    }
    int32_t edgeCursor = 0;
    for (int32_t edgeID = 0; edgeID < lv->edgeCount; ++edgeID) {
        for (int32_t r = 0; r < lv->edgeRefCount[edgeID]; ++r) {
            lv->reverseOffset[lv->edgeRefs[edgeCursor + r] - lv->readOffset + 1] += 1;
        }
        edgeCursor += lv->edgeRefCount[edgeID];
    }
    int32_t vertexCursor = 0;
    for (int32_t vertexID = 0; vertexID < lv->vertexCount; ++vertexID) {
        for (int32_t r = 0; r < lv->vertexRefCount[vertexID]; ++r) {
            const int32_t g = lv->vertexRefs[vertexCursor + r];
            lv->reverseOffset[g - lv->readOffset + 1] += 1;
        }
        vertexCursor += lv->vertexRefCount[vertexID];
    }

    for (int32_t i = 1; i <= span; ++i) {
        lv->reverseOffset[i] += lv->reverseOffset[i - 1];
    }
    CCE_ASSERT((size_t)lv->reverseOffset[span] == totalRefs);

    lv->reverseAdj = (int32_t *)CCE_MALLOC(totalRefs * sizeof(int32_t));
    if (lv->reverseAdj == NULL) {
        return 0;
    }

    /* fill pass: reverseOffset is used as a moving cursor and restored after */
    int32_t *fill = (int32_t *)CCE_MALLOC((size_t)span * sizeof(int32_t));
    if (fill == NULL) {
        return 0;
    }
    CCE_MEMCPY(fill, lv->reverseOffset, (size_t)span * sizeof(int32_t));

    faceCursor = 0;
    for (int32_t faceID = 0; faceID < lv->faceCount; ++faceID) {
        const int32_t outputGlobal = lv->faceOffset + faceID;
        for (int32_t r = 0; r < lv->faceRefCount[faceID]; ++r) {
            const int32_t slot = lv->faceRefs[faceCursor + r] - lv->readOffset;
            lv->reverseAdj[fill[slot]++] = outputGlobal;
        }
        faceCursor += lv->faceRefCount[faceID];
    }
    edgeCursor = 0;
    for (int32_t edgeID = 0; edgeID < lv->edgeCount; ++edgeID) {
        const int32_t outputGlobal = lv->edgeOffset + edgeID;
        for (int32_t r = 0; r < lv->edgeRefCount[edgeID]; ++r) {
            const int32_t slot = lv->edgeRefs[edgeCursor + r] - lv->readOffset;
            lv->reverseAdj[fill[slot]++] = outputGlobal;
        }
        edgeCursor += lv->edgeRefCount[edgeID];
    }
    vertexCursor = 0;
    for (int32_t vertexID = 0; vertexID < lv->vertexCount; ++vertexID) {
        const int32_t outputGlobal = lv->writeOffset + vertexID;
        for (int32_t r = 0; r < lv->vertexRefCount[vertexID]; ++r) {
            const int32_t slot = lv->vertexRefs[vertexCursor + r] - lv->readOffset;
            lv->reverseAdj[fill[slot]++] = outputGlobal;
        }
        vertexCursor += lv->vertexRefCount[vertexID];
    }

    CCE_FREE(fill);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Sparse rule evaluation (same arithmetic as the Gather path)        */
/* ------------------------------------------------------------------ */

/* ccs__CageFacePoints_Gather / ccs__FacePoints_Gather. */
static cc_VertexPoint cce__EvalFace(cc_EditPlan *plan, int32_t level, int32_t faceID)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t first = level == 0
        ? plan->cage->faceToHalfedgeIDs[faceID]
        : ccm_FaceToHalfedgeID_Quad(faceID);
    cc_VertexPoint acc = *cce__PointAt(
        plan, level, lv->readOffset + cce__VertexID(plan, level, first));
    int32_t h = cce__NextID(plan, level, first);
    float valence = 1.0f;

    while (h != first) {
        const cc_VertexPoint *p = cce__PointAt(
            plan, level, lv->readOffset + cce__VertexID(plan, level, h));
        acc.x += p->x;
        acc.y += p->y;
        acc.z += p->z;
        valence += 1.0f;
        h = cce__NextID(plan, level, h);
    }
    acc.x /= valence;
    acc.y /= valence;
    acc.z /= valence;

    return acc;
}

/* ccs__CreasedCageEdgePoints_Gather / ccs__CreasedEdgePoints_Gather. */
static cc_VertexPoint cce__EvalEdge(cc_EditPlan *plan, int32_t level, int32_t edgeID)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t h = level == 0
        ? plan->cage->edgeToHalfedgeIDs[edgeID]
        : ccs_EdgeToHalfedgeID(plan->subd, edgeID, level);
    const int32_t twinID = cce__TwinID(plan, level, h);
    const int32_t nextID = cce__NextID(plan, level, h);
    const float edgeWeight = cce__Satf(cce__EdgeSharpness(plan, level, edgeID));
    const cc_VertexPoint *p0 = cce__PointAt(
        plan, level, lv->readOffset + cce__VertexID(plan, level, h));
    const cc_VertexPoint *p1 = cce__PointAt(
        plan, level, lv->readOffset + cce__VertexID(plan, level, nextID));
    const cc_VertexPoint *f0 = cce__PointAt(
        plan, level, lv->faceOffset + cce__FaceID(plan, level, h));
    const cc_VertexPoint *f1 = cce__PointAt(
        plan, level, lv->faceOffset + cce__FaceID(plan, level, cce__MaxInt(0, twinID)));
    cc_VertexPoint sharpPoint, smoothPoint, result;
    float tmp1[3], tmp2[3];

    tmp1[0] = p0->x + p1->x;
    tmp1[1] = p0->y + p1->y;
    tmp1[2] = p0->z + p1->z;
    tmp2[0] = f0->x + f1->x;
    tmp2[1] = f0->y + f1->y;
    tmp2[2] = f0->z + f1->z;

    sharpPoint.x = tmp1[0] * 0.5f;
    sharpPoint.y = tmp1[1] * 0.5f;
    sharpPoint.z = tmp1[2] * 0.5f;
    smoothPoint.x = (tmp1[0] + tmp2[0]) * 0.25f;
    smoothPoint.y = (tmp1[1] + tmp2[1]) * 0.25f;
    smoothPoint.z = (tmp1[2] + tmp2[2]) * 0.25f;

    /* cc__Lerp3f(out, smooth, sharp, edgeWeight) of the creased edge rule */
    result.x = smoothPoint.x + edgeWeight * (sharpPoint.x - smoothPoint.x);
    result.y = smoothPoint.y + edgeWeight * (sharpPoint.y - smoothPoint.y);
    result.z = smoothPoint.z + edgeWeight * (sharpPoint.z - smoothPoint.z);

    return result;
}

/* ccs__CreasedCageVertexPoints_Gather. */
static cc_VertexPoint
cce__EvalCageVertex(cc_EditPlan *plan, int32_t vertexID)
{
    const cc_Mesh *cage = plan->cage;
    const cce__Level *lv = &plan->levels[0];
    const int32_t halfedgeID = cage->vertexToHalfedgeIDs[vertexID];
    const int32_t edgeID = cage->halfedges[halfedgeID].edgeID;
    const int32_t prevID = cage->halfedges[halfedgeID].prevID;
    const int32_t prevEdgeID = cage->halfedges[prevID].edgeID;
    const int32_t prevFaceID = cage->halfedges[prevID].faceID;
    const float thisS = cage->creases[edgeID].sharpness;
    const float prevS = cage->creases[prevEdgeID].sharpness;
    const float creaseWeight = cce__Signf(thisS);
    const float prevCreaseWeight = cce__Signf(prevS);
    const cc_VertexPoint *newEdgePoint = cce__PointAt(plan, 0, lv->edgeOffset + edgeID);
    const cc_VertexPoint *newPrevEdgePoint = cce__PointAt(plan, 0, lv->edgeOffset + prevEdgeID);
    const cc_VertexPoint *newPrevFacePoint = cce__PointAt(plan, 0, lv->faceOffset + prevFaceID);
    const cc_VertexPoint oldPoint = cage->vertexPoints[vertexID];
    cc_VertexPoint smoothPoint = {0.0f, 0.0f, 0.0f};
    cc_VertexPoint creasePoint = {0.0f, 0.0f, 0.0f};
    float avgS = prevS;
    float creaseCount = prevCreaseWeight;
    float valence = 1.0f;
    int32_t forwardIterator;
    float tmp1[3], tmp2[3];

    tmp1[0] = -newPrevFacePoint->x;
    tmp1[1] = -newPrevFacePoint->y;
    tmp1[2] = -newPrevFacePoint->z;
    tmp2[0] = 4.0f * newPrevEdgePoint->x;
    tmp2[1] = 4.0f * newPrevEdgePoint->y;
    tmp2[2] = 4.0f * newPrevEdgePoint->z;
    smoothPoint.x = tmp1[0] + tmp2[0];
    smoothPoint.y = tmp1[1] + tmp2[1];
    smoothPoint.z = tmp1[2] + tmp2[2];

    creasePoint.x = prevCreaseWeight * newPrevEdgePoint->x;
    creasePoint.y = prevCreaseWeight * newPrevEdgePoint->y;
    creasePoint.z = prevCreaseWeight * newPrevEdgePoint->z;

    for (forwardIterator = cage->halfedges[prevID].twinID;
         forwardIterator >= 0 && forwardIterator != halfedgeID;
         forwardIterator = cage->halfedges[forwardIterator].twinID) {
        const int32_t ringPrev = cage->halfedges[forwardIterator].prevID;
        const int32_t ringEdgeID = cage->halfedges[ringPrev].edgeID;
        const int32_t ringFaceID = cage->halfedges[ringPrev].faceID;
        const float ringS = cage->creases[ringEdgeID].sharpness;
        const float ringCreaseWeight = cce__Signf(ringS);
        const cc_VertexPoint *ringEdgePoint = cce__PointAt(plan, 0, lv->edgeOffset + ringEdgeID);
        const cc_VertexPoint *ringFacePoint = cce__PointAt(plan, 0, lv->faceOffset + ringFaceID);

        smoothPoint.x += -ringFacePoint->x + 4.0f * ringEdgePoint->x;
        smoothPoint.y += -ringFacePoint->y + 4.0f * ringEdgePoint->y;
        smoothPoint.z += -ringFacePoint->z + 4.0f * ringEdgePoint->z;
        ++valence;

        creasePoint.x += ringCreaseWeight * ringEdgePoint->x;
        creasePoint.y += ringCreaseWeight * ringEdgePoint->y;
        creasePoint.z += ringCreaseWeight * ringEdgePoint->z;
        avgS += ringS;
        creaseCount += ringCreaseWeight;

        forwardIterator = ringPrev;
    }

    if (forwardIterator < 0) {
        creasePoint.x += creaseWeight * newEdgePoint->x;
        creasePoint.y += creaseWeight * newEdgePoint->y;
        creasePoint.z += creaseWeight * newEdgePoint->z;
        creaseCount += creaseWeight;
        ++valence;
    }

    tmp1[0] = smoothPoint.x / (valence * valence);
    tmp1[1] = smoothPoint.y / (valence * valence);
    tmp1[2] = smoothPoint.z / (valence * valence);
    tmp2[0] = oldPoint.x * (1.0f - 3.0f / valence);
    tmp2[1] = oldPoint.y * (1.0f - 3.0f / valence);
    tmp2[2] = oldPoint.z * (1.0f - 3.0f / valence);
    smoothPoint.x = tmp1[0] + tmp2[0];
    smoothPoint.y = tmp1[1] + tmp2[1];
    smoothPoint.z = tmp1[2] + tmp2[2];

    tmp1[0] = creasePoint.x * 0.25f;
    tmp1[1] = creasePoint.y * 0.25f;
    tmp1[2] = creasePoint.z * 0.25f;
    tmp2[0] = oldPoint.x * 0.5f;
    tmp2[1] = oldPoint.y * 0.5f;
    tmp2[2] = oldPoint.z * 0.5f;
    creasePoint.x = tmp1[0] + tmp2[0];
    creasePoint.y = tmp1[1] + tmp2[1];
    creasePoint.z = tmp1[2] + tmp2[2];

    if (creaseCount <= 1.0f) {
        return smoothPoint;
    } else if (creaseCount >= 3.0f || valence == 2.0f) {
        return oldPoint;
    }
    {
        const float w = cce__Satf(avgS * 0.5f);
        cc_VertexPoint result;
        result.x = oldPoint.x + w * (creasePoint.x - oldPoint.x);
        result.y = oldPoint.y + w * (creasePoint.y - oldPoint.y);
        result.z = oldPoint.z + w * (creasePoint.z - oldPoint.z);
        return result;
    }
}

/* ccs__CreasedVertexPoints_Gather. */
static cc_VertexPoint
cce__EvalSubdVertex(cc_EditPlan *plan, int32_t level, int32_t vertexID)
{
    const cce__Level *lv = &plan->levels[level];
    const int32_t halfedgeID = cce__VertexToHalfedgeID(plan->subd, vertexID, level);
    const int32_t edgeID = cce__EdgeID(plan, level, halfedgeID);
    const int32_t prevID = cce__PrevID(plan, level, halfedgeID);
    const int32_t prevEdgeID = cce__EdgeID(plan, level, prevID);
    const int32_t prevFaceID = cce__FaceID(plan, level, prevID);
    const float thisS = cce__EdgeSharpness(plan, level, edgeID);
    const float prevS = cce__EdgeSharpness(plan, level, prevEdgeID);
    const float creaseWeight = cce__Signf(thisS);
    const float prevCreaseWeight = cce__Signf(prevS);
    const cc_VertexPoint *newEdgePoint = cce__PointAt(plan, level, lv->edgeOffset + edgeID);
    const cc_VertexPoint *newPrevEdgePoint = cce__PointAt(plan, level, lv->edgeOffset + prevEdgeID);
    const cc_VertexPoint *newPrevFacePoint = cce__PointAt(plan, level, lv->faceOffset + prevFaceID);
    const cc_VertexPoint oldPoint = plan->subd->vertexPoints[lv->readOffset + vertexID];
    cc_VertexPoint smoothPoint = {0.0f, 0.0f, 0.0f};
    cc_VertexPoint creasePoint = {0.0f, 0.0f, 0.0f};
    float avgS = prevS;
    float creaseCount = prevCreaseWeight;
    float valence = 1.0f;
    int32_t forwardIterator, backwardIterator;
    float tmp1[3], tmp2[3];

    smoothPoint.x = -newPrevFacePoint->x + 4.0f * newPrevEdgePoint->x;
    smoothPoint.y = -newPrevFacePoint->y + 4.0f * newPrevEdgePoint->y;
    smoothPoint.z = -newPrevFacePoint->z + 4.0f * newPrevEdgePoint->z;
    creasePoint.x = prevCreaseWeight * newPrevEdgePoint->x;
    creasePoint.y = prevCreaseWeight * newPrevEdgePoint->y;
    creasePoint.z = prevCreaseWeight * newPrevEdgePoint->z;

    for (forwardIterator = cce__TwinID(plan, level, prevID);
         forwardIterator >= 0 && forwardIterator != halfedgeID;
         forwardIterator = cce__TwinID(plan, level, forwardIterator)) {
        const int32_t ringPrev = cce__PrevID(plan, level, forwardIterator);
        const int32_t ringEdgeID = cce__EdgeID(plan, level, ringPrev);
        const int32_t ringFaceID = cce__FaceID(plan, level, ringPrev);
        const float ringS = cce__EdgeSharpness(plan, level, ringEdgeID);
        const float ringCreaseWeight = cce__Signf(ringS);
        const cc_VertexPoint *ringEdgePoint = cce__PointAt(plan, level, lv->edgeOffset + ringEdgeID);
        const cc_VertexPoint *ringFacePoint = cce__PointAt(plan, level, lv->faceOffset + ringFaceID);

        smoothPoint.x += -ringFacePoint->x + 4.0f * ringEdgePoint->x;
        smoothPoint.y += -ringFacePoint->y + 4.0f * ringEdgePoint->y;
        smoothPoint.z += -ringFacePoint->z + 4.0f * ringEdgePoint->z;
        ++valence;

        creasePoint.x += ringCreaseWeight * ringEdgePoint->x;
        creasePoint.y += ringCreaseWeight * ringEdgePoint->y;
        creasePoint.z += ringCreaseWeight * ringEdgePoint->z;
        avgS += ringS;
        creaseCount += ringCreaseWeight;

        forwardIterator = ringPrev;
    }

    for (backwardIterator = cce__TwinID(plan, level, halfedgeID);
         forwardIterator < 0 && backwardIterator >= 0 && backwardIterator != halfedgeID;
         backwardIterator = cce__TwinID(plan, level, backwardIterator)) {
        const int32_t ringNext = cce__NextID(plan, level, backwardIterator);
        const int32_t ringEdgeID = cce__EdgeID(plan, level, ringNext);
        const int32_t ringFaceID = cce__FaceID(plan, level, ringNext);
        const float ringS = cce__EdgeSharpness(plan, level, ringEdgeID);
        const float ringCreaseWeight = cce__Signf(ringS);
        const cc_VertexPoint *ringEdgePoint = cce__PointAt(plan, level, lv->edgeOffset + ringEdgeID);
        const cc_VertexPoint *ringFacePoint = cce__PointAt(plan, level, lv->faceOffset + ringFaceID);

        smoothPoint.x += -ringFacePoint->x + 4.0f * ringEdgePoint->x;
        smoothPoint.y += -ringFacePoint->y + 4.0f * ringEdgePoint->y;
        smoothPoint.z += -ringFacePoint->z + 4.0f * ringEdgePoint->z;
        ++valence;

        creasePoint.x += ringCreaseWeight * ringEdgePoint->x;
        creasePoint.y += ringCreaseWeight * ringEdgePoint->y;
        creasePoint.z += ringCreaseWeight * ringEdgePoint->z;
        avgS += ringS;
        creaseCount += ringCreaseWeight;

        backwardIterator = ringNext;
    }

    if (forwardIterator < 0) {
        creasePoint.x += creaseWeight * newEdgePoint->x;
        creasePoint.y += creaseWeight * newEdgePoint->y;
        creasePoint.z += creaseWeight * newEdgePoint->z;
        creaseCount += creaseWeight;
        ++valence;
    }

    tmp1[0] = smoothPoint.x / (valence * valence);
    tmp1[1] = smoothPoint.y / (valence * valence);
    tmp1[2] = smoothPoint.z / (valence * valence);
    tmp2[0] = oldPoint.x * (1.0f - 3.0f / valence);
    tmp2[1] = oldPoint.y * (1.0f - 3.0f / valence);
    tmp2[2] = oldPoint.z * (1.0f - 3.0f / valence);
    smoothPoint.x = tmp1[0] + tmp2[0];
    smoothPoint.y = tmp1[1] + tmp2[1];
    smoothPoint.z = tmp1[2] + tmp2[2];

    tmp1[0] = creasePoint.x * (0.5f / creaseCount);
    tmp1[1] = creasePoint.y * (0.5f / creaseCount);
    tmp1[2] = creasePoint.z * (0.5f / creaseCount);
    tmp2[0] = oldPoint.x * 0.5f;
    tmp2[1] = oldPoint.y * 0.5f;
    tmp2[2] = oldPoint.z * 0.5f;
    creasePoint.x = tmp1[0] + tmp2[0];
    creasePoint.y = tmp1[1] + tmp2[1];
    creasePoint.z = tmp1[2] + tmp2[2];

    if (creaseCount <= 1.0f) {
        return smoothPoint;
    } else if (creaseCount >= 3.0f || valence == 2.0f) {
        return oldPoint;
    }
    {
        const float w = cce__Satf(avgS * 0.5f);
        cc_VertexPoint result;
        result.x = oldPoint.x + w * (creasePoint.x - oldPoint.x);
        result.y = oldPoint.y + w * (creasePoint.y - oldPoint.y);
        result.z = oldPoint.z + w * (creasePoint.z - oldPoint.z);
        return result;
    }
}

/* ------------------------------------------------------------------ */
/* Footprint propagation                                              */
/* ------------------------------------------------------------------ */

static void cce__NextMarkGeneration(cc_EditPlan *plan)
{
    if (plan->markGeneration == INT32_MAX) {
        CCE_MEMSET(plan->marks, 0, (size_t)plan->totalVertexCount * sizeof(int32_t));
        plan->markGeneration = 0;
    }
    plan->markGeneration += 1;
}

/*
 * Add every (unique) output row in [lo,hi) that depends on one of the input
 * rows, using the reverse index. New rows are appended to plan->dirty and
 * stamped with the current mark generation.
 */
static void
cce__CollectDependents(cc_EditPlan *plan, const cce__Level *lv,
                       const int32_t *inputs, int32_t inputCount,
                       int32_t lo, int32_t hi, int32_t *dirtyCount)
{
    for (int32_t i = 0; i < inputCount; ++i) {
        const int32_t slot = inputs[i] - lv->readOffset;
        if (slot < 0 || slot >= lv->reverseSpan) {
            continue;
        }
        const int32_t begin = lv->reverseOffset[slot];
        const int32_t end = lv->reverseOffset[slot + 1];
        for (int32_t r = begin; r < end; ++r) {
            const int32_t dependent = lv->reverseAdj[r];
            if (dependent < lo || dependent >= hi) {
                continue;
            }
            if (plan->marks[dependent] != plan->markGeneration) {
                plan->marks[dependent] = plan->markGeneration;
                plan->dirty[(*dirtyCount)++] = dependent;
            }
        }
    }
}

/*
 * Propagate one subdivision step.
 *
 * Propagation is role-layered in the exact Gather rule order rather than an
 * undirected graph flood: face points are derived from the changed old
 * vertices, edge points from old vertices plus the new face points, and vertex
 * points from old vertices plus face and edge points. Layering keeps the
 * footprint local -- treating new and old vertex rows as mutually reachable
 * would re-enter through alias rows and flood an entire connected component.
 *
 * The seed rows occupy plan->dirty[0..seedCount) and are preserved; evaluated
 * output rows are appended after them and then compacted to the front, so on
 * return plan->dirty[0..result) holds the unique changed output rows (the
 * read-range seeds of the next level).
 */
static int cce__PropagateLevel(cc_EditPlan *plan, int32_t level,
                               const int32_t *seeds, int32_t seedCount,
                               size_t *visitedRows, size_t *evaluatedRows)
{
    const cce__Level *lv = &plan->levels[level];
    int32_t dirtyCount = seedCount;
    int32_t faceBegin, faceEnd, edgeEnd;

    cce__NextMarkGeneration(plan);

    /* stamp the seed rows so output collection never re-adds them */
    for (int32_t i = 0; i < seedCount; ++i) {
        plan->marks[seeds[i]] = plan->markGeneration;
    }

    /* layer 1: face points depend only on old vertices */
    faceBegin = dirtyCount;
    cce__CollectDependents(plan, lv, seeds, seedCount,
                           lv->faceOffset, lv->edgeOffset, &dirtyCount);
    faceEnd = dirtyCount;
    for (int32_t i = faceBegin; i < faceEnd; ++i) {
        const int32_t globalID = plan->dirty[i];
        cce__WritePoint(plan, level, globalID,
                        cce__EvalFace(plan, level, globalID - lv->faceOffset));
    }

    /* layer 2: edge points depend on old vertices and new face points */
    cce__CollectDependents(plan, lv, seeds, seedCount,
                           lv->edgeOffset, lv->blockEnd, &dirtyCount);
    cce__CollectDependents(plan, lv, plan->dirty + faceBegin, faceEnd - faceBegin,
                           lv->edgeOffset, lv->blockEnd, &dirtyCount);
    edgeEnd = dirtyCount;
    for (int32_t i = faceEnd; i < edgeEnd; ++i) {
        const int32_t globalID = plan->dirty[i];
        cce__WritePoint(plan, level, globalID,
                        cce__EvalEdge(plan, level, globalID - lv->edgeOffset));
    }

    /* layer 3: vertex points depend on old vertices, face and edge points */
    cce__CollectDependents(plan, lv, seeds, seedCount,
                           lv->writeOffset, lv->faceOffset, &dirtyCount);
    cce__CollectDependents(plan, lv, plan->dirty + faceBegin, faceEnd - faceBegin,
                           lv->writeOffset, lv->faceOffset, &dirtyCount);
    cce__CollectDependents(plan, lv, plan->dirty + faceEnd, edgeEnd - faceEnd,
                           lv->writeOffset, lv->faceOffset, &dirtyCount);

    /*
     * At level 0 the new vertex rows alias the cage control rows: a moved cage
     * vertex is itself a vertex-rule output, but dedup kept it out of the
     * appended (non-seed) set. Evaluate those seed vertex rows explicitly.
     */
    if (level == 0) {
        for (int32_t i = 0; i < seedCount; ++i) {
            const int32_t globalID = seeds[i];
            cce__WritePoint(plan, 0, globalID,
                            cce__EvalCageVertex(plan, globalID - lv->writeOffset));
        }
    }
    for (int32_t i = edgeEnd; i < dirtyCount; ++i) {
        const int32_t globalID = plan->dirty[i];
        if (level == 0) {
            cce__WritePoint(plan, level, globalID,
                            cce__EvalCageVertex(plan, globalID - lv->writeOffset));
        } else {
            cce__WritePoint(plan, level, globalID,
                            cce__EvalSubdVertex(plan, level,
                                                globalID - lv->writeOffset));
        }
    }

    /*
     * Build the next level's seed set. At level 0 the seed cage rows are also
     * output rows inside the depth-1 block, so they stay; at deeper levels the
     * input rows live in a separate block and only appended outputs carry over.
     */
    int32_t outputCount;
    if (level == 0) {
        outputCount = dirtyCount; /* seeds + outputs, all re-evaluated above */
    } else {
        outputCount = dirtyCount - seedCount;
        for (int32_t i = 0; i < outputCount; ++i) {
            plan->dirty[i] = plan->dirty[seedCount + i];
        }
    }

    /* Every unique candidate output row was visited and evaluated once. */
    *visitedRows += (size_t)outputCount;
    *evaluatedRows += (size_t)outputCount;

    return outputCount;
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

static void cce__FreeLevel(cce__Level *lv)
{
    CCE_FREE(lv->faceRefs);
    CCE_FREE(lv->edgeRefs);
    CCE_FREE(lv->vertexRefs);
    CCE_FREE(lv->faceRefCount);
    CCE_FREE(lv->edgeRefCount);
    CCE_FREE(lv->vertexRefCount);
    CCE_FREE(lv->reverseOffset);
    CCE_FREE(lv->reverseAdj);
    CCE_MEMSET(lv, 0, sizeof(*lv));
}

/*
 * Forward dependency storage is only consumed while building the reverse
 * index. Runtime propagation reads the reverse index and the (fixed)
 * topology, so the forward arrays can be released once construction finishes.
 */
static void cce__ReleaseForwardRefs(cce__Level *lv)
{
    CCE_FREE(lv->faceRefs);
    CCE_FREE(lv->edgeRefs);
    CCE_FREE(lv->vertexRefs);
    CCE_FREE(lv->faceRefCount);
    CCE_FREE(lv->edgeRefCount);
    CCE_FREE(lv->vertexRefCount);
    lv->faceRefs = lv->edgeRefs = lv->vertexRefs = NULL;
    lv->faceRefCount = lv->edgeRefCount = lv->vertexRefCount = NULL;
    lv->faceRefCapacity = lv->edgeRefCapacity = lv->vertexRefCapacity = 0;
}

static cc_Mesh *cce__CopyMesh(const cc_Mesh *src)
{
    cc_Mesh *dst = ccm_Create(src->vertexCount,
                              src->uvCount,
                              src->halfedgeCount,
                              src->edgeCount,
                              src->faceCount);
    if (dst == NULL ||
        dst->vertexToHalfedgeIDs == NULL || dst->edgeToHalfedgeIDs == NULL ||
        dst->faceToHalfedgeIDs == NULL || dst->halfedges == NULL ||
        dst->creases == NULL || dst->vertexPoints == NULL ||
        (src->uvCount > 0 && dst->uvs == NULL)) {
        if (dst != NULL) {
            ccm_Release(dst);
        }
        return NULL;
    }

    CCE_MEMCPY(dst->vertexToHalfedgeIDs, src->vertexToHalfedgeIDs,
               (size_t)src->vertexCount * sizeof(int32_t));
    CCE_MEMCPY(dst->edgeToHalfedgeIDs, src->edgeToHalfedgeIDs,
               (size_t)src->edgeCount * sizeof(int32_t));
    CCE_MEMCPY(dst->faceToHalfedgeIDs, src->faceToHalfedgeIDs,
               (size_t)src->faceCount * sizeof(int32_t));
    CCE_MEMCPY(dst->vertexPoints, src->vertexPoints,
               (size_t)src->vertexCount * sizeof(cc_VertexPoint));
    if (src->uvCount > 0) {
        CCE_MEMCPY(dst->uvs, src->uvs,
                   (size_t)src->uvCount * sizeof(cc_VertexUv));
    }
    CCE_MEMCPY(dst->halfedges, src->halfedges,
               (size_t)src->halfedgeCount * sizeof(cc_Halfedge));
    CCE_MEMCPY(dst->creases, src->creases,
               (size_t)src->edgeCount * sizeof(cc_Crease));

    return dst;
}

cc_EditPlan *ccs_EditPlanCreate(const cc_Mesh *cage, int32_t depth)
{
    if (cage == NULL || depth < 1 || depth > CCE_MAX_DEPTH) {
        return NULL;
    }
    if (cage->vertexCount <= 0 || cage->halfedgeCount <= 0 ||
        cage->faceCount <= 0 || cage->edgeCount <= 0) {
        return NULL;
    }

    cc_EditPlan *plan = (cc_EditPlan *)CCE_MALLOC(sizeof(*plan));
    if (plan == NULL) {
        return NULL;
    }
    CCE_MEMSET(plan, 0, sizeof(*plan));
    plan->depth = depth;
    plan->cageVertexCount = cage->vertexCount;

    plan->cage = cce__CopyMesh(cage);
    if (plan->cage == NULL) {
        ccs_EditPlanRelease(plan);
        return NULL;
    }

    plan->subd = ccs_Create(plan->cage, depth);
    if (plan->subd == NULL) {
        ccs_EditPlanRelease(plan);
        return NULL;
    }
    /* Full initial subdivision: topology, creases, UVs and positions. */
    ccs_Refine_Gather(plan->subd);

    plan->totalVertexCount = ccs_CumulativeVertexCountAtDepth(plan->cage, depth);
    plan->marks = (int32_t *)CCE_MALLOC((size_t)plan->totalVertexCount * sizeof(int32_t));
    plan->dupMarks = (int32_t *)CCE_MALLOC((size_t)plan->cageVertexCount * sizeof(int32_t));
    plan->dirty = (int32_t *)CCE_MALLOC((size_t)plan->totalVertexCount * sizeof(int32_t));
    if (plan->marks == NULL || plan->dupMarks == NULL || plan->dirty == NULL) {
        ccs_EditPlanRelease(plan);
        return NULL;
    }
    CCE_MEMSET(plan->marks, 0, (size_t)plan->totalVertexCount * sizeof(int32_t));
    CCE_MEMSET(plan->dupMarks, 0, (size_t)plan->cageVertexCount * sizeof(int32_t));
    plan->dupGeneration = 0;

    for (int32_t level = 0; level < depth; ++level) {
        cce__Level *lv = &plan->levels[level];

        if (level == 0) {
            lv->vertexCount = cage->vertexCount;
            lv->edgeCount = cage->edgeCount;
            lv->faceCount = cage->faceCount;
            lv->creaseCount = cage->edgeCount;
            lv->readOffset = 0;
            lv->writeOffset = 0;
            lv->faceOffset = cage->vertexCount;
            lv->edgeOffset = cage->vertexCount + cage->faceCount;
            lv->halfedgeStride = 0;
            lv->creaseStride = 0;
        } else {
            lv->vertexCount = ccm_VertexCountAtDepth_Fast(cage, level);
            lv->edgeCount = ccm_EdgeCountAtDepth_Fast(cage, level);
            lv->faceCount = ccm_FaceCountAtDepth_Fast(cage, level);
            lv->creaseCount = ccm_CreaseCountAtDepth(cage, level);
            lv->readOffset = ccs_CumulativeVertexCountAtDepth(cage, level - 1);
            lv->writeOffset = ccs_CumulativeVertexCountAtDepth(cage, level);
            lv->faceOffset = lv->writeOffset + lv->vertexCount;
            lv->edgeOffset = lv->faceOffset + lv->faceCount;
            lv->halfedgeStride = ccs_CumulativeHalfedgeCountAtDepth(cage, level - 1);
            lv->creaseStride = ccs_CumulativeCreaseCountAtDepth(cage, level - 1);
        }
        lv->blockEnd = lv->edgeOffset + lv->edgeCount;

        if (!cce__BuildLevelCache(plan, level) ||
            !cce__BuildReverseIndex(plan, level)) {
            ccs_EditPlanRelease(plan);
            return NULL;
        }
        /* Forward refs only seed the reverse index; runtime propagation and
         * evaluation use the reverse index and the topology, so drop them. */
        cce__ReleaseForwardRefs(&plan->levels[level]);
    }

    return plan;
}

void ccs_EditPlanRelease(cc_EditPlan *plan)
{
    if (plan == NULL) {
        return;
    }
    for (int32_t level = 0; level < CCE_MAX_DEPTH; ++level) {
        cce__FreeLevel(&plan->levels[level]);
    }
    CCE_FREE(plan->marks);
    CCE_FREE(plan->dupMarks);
    CCE_FREE(plan->dirty);
    if (plan->subd != NULL) {
        ccs_Release(plan->subd);
    }
    if (plan->cage != NULL) {
        ccm_Release(plan->cage);
    }
    CCE_FREE(plan);
}

const cc_Subd *ccs_EditPlanSubd(const cc_EditPlan *plan)
{
    return plan != NULL ? plan->subd : NULL;
}

/* ------------------------------------------------------------------ */
/* Update entry point                                                 */
/* ------------------------------------------------------------------ */

int ccs_EditPlanUpdate(cc_EditPlan *plan,
                       const int32_t *ids,
                       const cc_VertexPoint *positions,
                       size_t count,
                       cc_EditStats *stats)
{
    if (plan == NULL) {
        return 1;
    }
    if (count > 0 && (ids == NULL || positions == NULL)) {
        return 2;
    }
    if (count > (size_t)plan->cageVertexCount) {
        return 3;
    }

    /* Validate the whole batch before touching any stored position. */
    if (plan->dupGeneration == INT32_MAX) {
        CCE_MEMSET(plan->dupMarks, 0,
                   (size_t)plan->cageVertexCount * sizeof(int32_t));
        plan->dupGeneration = 0;
    }
    plan->dupGeneration += 1;
    for (size_t i = 0; i < count; ++i) {
        const int32_t id = ids[i];
        if (id < 0 || id >= plan->cageVertexCount) {
            return 4;
        }
        if (plan->dupMarks[id] == plan->dupGeneration) {
            return 5; /* duplicated index */
        }
        plan->dupMarks[id] = plan->dupGeneration;
        if (!isfinite(positions[i].x) || !isfinite(positions[i].y) ||
            !isfinite(positions[i].z)) {
            return 6;
        }
    }

    cc_EditStats localStats;
    CCE_MEMSET(&localStats, 0, sizeof(localStats));

    /* Level-0 seeds: only cage vertices whose absolute position actually moves.
     * Only the raw cage control point is written here; the matching depth-1
     * vertex row (same global id in the subd buffer) is re-evaluated through
     * the reverse index by the cage vertex rule. */
    int32_t seedCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t id = ids[i];
        const cc_VertexPoint oldPoint = plan->cage->vertexPoints[id];
        if (oldPoint.x != positions[i].x ||
            oldPoint.y != positions[i].y ||
            oldPoint.z != positions[i].z) {
            plan->cage->vertexPoints[id] = positions[i];
            plan->dirty[seedCount++] = id;
        }
    }

    for (int32_t level = 0; level < plan->depth && seedCount > 0; ++level) {
        size_t visited = 0;
        size_t evaluated = 0;
        int32_t nextSeedCount;

        nextSeedCount = cce__PropagateLevel(plan, level,
                                            plan->dirty, seedCount,
                                            &visited, &evaluated);

        /* Counters are per output depth: level L produces the rows of
         * depth L + 1. Entries beyond plan->depth stay zero. */
        localStats.visited_rows[level] = visited;
        localStats.evaluated_rows[level] = evaluated;
        seedCount = nextSeedCount;
    }

    if (stats != NULL) {
        *stats = localStats;
    }

    return 0;
}

#endif // CC_EDIT_IMPLEMENTATION
