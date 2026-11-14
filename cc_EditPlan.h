#ifndef CC_INCLUDE_CC_EDIT_PLAN_H
#define CC_INCLUDE_CC_EDIT_PLAN_H

#include "CatmullClark.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CC_EDIT_STATIC
#define CCEDEF static
#else
#define CCEDEF extern
#endif

#include <stddef.h>

// per-update statistics, indexed by output depth (slot 0 == depth 1)
typedef struct {
    size_t evaluated_rows[5]; // position rows actually re-evaluated
    size_t visited_rows[5];   // candidate rows visited during propagation
} cc_EditStats;

// opaque sparse edit plan
typedef struct cc_EditPlan cc_EditPlan;

// ctor / dtor
CCEDEF cc_EditPlan *ccs_EditPlanCreate(const cc_Mesh *cage, int32_t depth);
CCEDEF void ccs_EditPlanRelease(cc_EditPlan *plan);

// plan-owned read-only subdivision result
CCEDEF const cc_Subd *ccs_EditPlanSubd(const cc_EditPlan *plan);

// sparse position update; returns 0 on success, non-zero on rejection
CCEDEF int ccs_EditPlanUpdate(cc_EditPlan *plan,
                              const int32_t *ids,
                              const cc_VertexPoint *positions,
                              size_t count,
                              cc_EditStats *stats);

#ifdef __cplusplus
} // extern "C"
#endif


//
//
//// end header file ///////////////////////////////////////////////////////////
#endif // CC_INCLUDE_CC_EDIT_PLAN_H

#ifdef CC_EDIT_IMPLEMENTATION

#ifndef CC_EDIT_ASSERT
#    include <assert.h>
#    define CC_EDIT_ASSERT(x) assert(x)
#endif

#ifndef CC_EDIT_MALLOC
#    include <stdlib.h>
#    define CC_EDIT_MALLOC(x) (malloc(x))
#    define CC_EDIT_FREE(x) (free(x))
#else
#    ifndef CC_EDIT_FREE
#        error CC_EDIT_MALLOC defined without CC_EDIT_FREE
#    endif
#endif

#ifndef CC_EDIT_MEMCPY
#    include <string.h>
#    define CC_EDIT_MEMCPY(dest, src, count) memcpy(dest, src, count)
#endif

#ifndef CC_EDIT_MEMSET
#    include <string.h>
#    define CC_EDIT_MEMSET(ptr, value, num) memset(ptr, value, num)
#endif

#include <math.h>
#include <string.h>

// exported by CatmullClark.h's implementation section (as
// ccs_VertexPointToHalfedgeID) but not declared in its public section
extern int32_t
ccs_VertexPointToHalfedgeID(const cc_Subd *subd, int32_t vertexID, int32_t depth);


/*******************************************************************************
 * Utility functions
 *
 */
static int32_t cce__Max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static float cce__Minf(float x, float y)
{
    return x < y ? x : y;
}

static float cce__Maxf(float x, float y)
{
    return x > y ? x : y;
}

static float cce__Satf(float x)
{
    return cce__Maxf(0.0f, cce__Minf(x, 1.0f));
}

static float cce__Signf(float x)
{
    if (x < 0.0f) {
        return -1.0f;
    } else if (x > 0.0f) {
        return +1.0f;
    }

    return 0.0f;
}

#define CCE__TRY(x) do { if ((x) != 0) return -1; } while (0)


/*******************************************************************************
 * Stencil -- Sparse affine combination of input rows
 *
 * Each output position row of a subdivision level is a fixed affine
 * combination of "input rows": the vertex rows of the parent level (ids in
 * [0, parentCount)) and the face / edge rows of the same level (ids in
 * [parentCount, rowCount), i.e. the row index itself). Since the topology
 * and the creases of a plan never change, the weights of these combinations
 * are frozen at plan creation by mirroring the upstream Gather rules
 * symbolically.
 */
typedef struct {
    int32_t id;
    float w;
} cce__Entry;

typedef struct {
    cce__Entry *entries;
    int32_t count;
    int32_t capacity;
} cce__Stencil;

static void cce__StInit(cce__Stencil *s)
{
    s->entries = NULL;
    s->count = 0;
    s->capacity = 0;
}

static void cce__StRelease(cce__Stencil *s)
{
    CC_EDIT_FREE(s->entries);
    cce__StInit(s);
}

static void cce__StReset(cce__Stencil *s)
{
    s->count = 0;
}

static int cce__StPush(cce__Stencil *s, int32_t id, float w)
{
    if (w == 0.0f) {
        return 0; // zero weights carry no dependency
    }
    for (int32_t i = 0; i < s->count; ++i) {
        if (s->entries[i].id == id) {
            const float nw = s->entries[i].w + w;

            if (nw == 0.0f) {
                s->entries[i] = s->entries[s->count - 1];
                --s->count;
            } else {
                s->entries[i].w = nw;
            }

            return 0;
        }
    }
    if (s->count == s->capacity) {
        const int32_t capacity = s->capacity > 0 ? 2 * s->capacity : 8;
        cce__Entry *entries =
            (cce__Entry *)CC_EDIT_MALLOC(sizeof(*entries) * (size_t)capacity);

        if (!entries) {
            return -1;
        }
        if (s->entries) {
            CC_EDIT_MEMCPY(entries, s->entries, sizeof(*entries) * (size_t)s->count);
            CC_EDIT_FREE(s->entries);
        }
        s->entries = entries;
        s->capacity = capacity;
    }
    s->entries[s->count].id = id;
    s->entries[s->count].w = w;
    ++s->count;

    return 0;
}

// dst+= w * src (dst must not alias src)
static int cce__StAcc(cce__Stencil *dst, const cce__Stencil *src, float w)
{
    if (w == 0.0f) {
        return 0;
    }
    for (int32_t i = 0; i < src->count; ++i) {
        CCE__TRY(cce__StPush(dst, src->entries[i].id, w * src->entries[i].w));
    }

    return 0;
}

static int cce__StCopy(cce__Stencil *dst, const cce__Stencil *src)
{
    cce__StReset(dst);

    return cce__StAcc(dst, src, 1.0f);
}

static int cce__StRef(cce__Stencil *dst, int32_t id)
{
    cce__StReset(dst);

    return cce__StPush(dst, id, 1.0f);
}

static void cce__StScale(cce__Stencil *s, float w)
{
    if (w == 0.0f) {
        s->count = 0;
    } else {
        for (int32_t i = 0; i < s->count; ++i) {
            s->entries[i].w*= w;
        }
    }
}

// dst = a + u (b - a) (dst must not alias a or b)
static int
cce__StLerp(cce__Stencil *dst, const cce__Stencil *a, const cce__Stencil *b, float u)
{
    cce__StReset(dst);
    CCE__TRY(cce__StAcc(dst, a, 1.0f - u));

    return cce__StAcc(dst, b, u);
}


/*******************************************************************************
 * LevelBuild -- Incremental construction of one level's forward stencils
 *
 */
typedef struct {
    int32_t parentCount;
    int32_t rowCount;
    int32_t rowCursor;
    size_t *offsets;
    cce__Entry *entries;
    size_t entryCount;
    size_t entryCapacity;
} cce__LevelBuild;

static int cce__LbInit(cce__LevelBuild *lb, int32_t parentCount, int32_t rowCount)
{
    lb->parentCount = parentCount;
    lb->rowCount = rowCount;
    lb->rowCursor = 0;
    lb->offsets = (size_t *)CC_EDIT_MALLOC(sizeof(size_t) * (size_t)(rowCount + 1));
    lb->entries = NULL;
    lb->entryCount = 0;
    lb->entryCapacity = 0;
    if (!lb->offsets) {
        return -1;
    }
    lb->offsets[0] = 0;

    return 0;
}

static void cce__LbRelease(cce__LevelBuild *lb)
{
    CC_EDIT_FREE(lb->offsets);
    CC_EDIT_FREE(lb->entries);
    lb->offsets = NULL;
    lb->entries = NULL;
}

static int cce__LbPushRow(cce__LevelBuild *lb, const cce__Stencil *st)
{
    CC_EDIT_ASSERT(lb->rowCursor < lb->rowCount);
    if (lb->entryCount + (size_t)st->count > lb->entryCapacity) {
        size_t capacity = lb->entryCapacity > 0 ? 2 * lb->entryCapacity : 256;
        cce__Entry *entries;

        while (capacity < lb->entryCount + (size_t)st->count) {
            capacity*= 2;
        }
        entries = (cce__Entry *)CC_EDIT_MALLOC(sizeof(*entries) * capacity);
        if (!entries) {
            return -1;
        }
        if (lb->entries) {
            CC_EDIT_MEMCPY(entries, lb->entries, sizeof(*entries) * lb->entryCount);
            CC_EDIT_FREE(lb->entries);
        }
        lb->entries = entries;
        lb->entryCapacity = capacity;
    }
    CC_EDIT_MEMCPY(lb->entries + lb->entryCount,
                   st->entries,
                   sizeof(cce__Entry) * (size_t)st->count);
    lb->entryCount+= (size_t)st->count;
    lb->offsets[lb->rowCursor + 1] = lb->entryCount;
    ++lb->rowCursor;

    return 0;
}


/*******************************************************************************
 * Edit plan data-structure
 *
 */
typedef struct {
    int32_t parentCount;  // vertex row count of the parent level
    int32_t rowCount;     // position row count of this level
    size_t *offsets;      // forward stencils, [rowCount + 1]
    cce__Entry *entries;
    size_t *revOffsets;   // reverse index, [rowCount + 1]
    int32_t *revRows;
} cce__Level;

struct cc_EditPlan {
    cc_Mesh *cage;        // private copy of the control cage
    cc_Subd *subd;        // full subdivision result (topology + positions)
    int32_t depth;
    cce__Level levels[5];
    uint32_t *cageGen;    // scratch generation flags over cage vertices
    uint32_t *rowGen;     // scratch generation flags over cumulative rows
    uint32_t generation;
    int32_t *bufA;        // scratch id lists (ping-pong)
    int32_t *bufB;
};


/*******************************************************************************
 * BuildReverse -- Builds the row -> dependent rows index of a level
 *
 * The stencil entry ids double as node ids of the reverse index: parent
 * vertex rows occupy [0, parentCount) and same-level rows keep their own
 * row index (always >= parentCount when referenced).
 */
static int cce__LevelBuildReverse(cce__Level *lev)
{
    const int32_t nodeCount = lev->rowCount;
    const size_t total = lev->offsets[lev->rowCount];
    size_t *cursor;

    lev->revOffsets = (size_t *)CC_EDIT_MALLOC(sizeof(size_t) * (size_t)(nodeCount + 1));
    lev->revRows = (int32_t *)CC_EDIT_MALLOC(sizeof(int32_t) * (total > 0 ? total : 1));
    if (!lev->revOffsets || !lev->revRows) {
        return -1;
    }
    CC_EDIT_MEMSET(lev->revOffsets, 0, sizeof(size_t) * (size_t)(nodeCount + 1));
    for (int32_t r = 0; r < lev->rowCount; ++r) {
        for (size_t j = lev->offsets[r]; j < lev->offsets[r + 1]; ++j) {
            ++lev->revOffsets[lev->entries[j].id + 1];
        }
    }
    for (int32_t n = 0; n < nodeCount; ++n) {
        lev->revOffsets[n + 1]+= lev->revOffsets[n];
    }
    cursor = (size_t *)CC_EDIT_MALLOC(sizeof(size_t) * (size_t)nodeCount);
    if (!cursor) {
        return -1;
    }
    CC_EDIT_MEMCPY(cursor, lev->revOffsets, sizeof(size_t) * (size_t)nodeCount);
    for (int32_t r = 0; r < lev->rowCount; ++r) {
        for (size_t j = lev->offsets[r]; j < lev->offsets[r + 1]; ++j) {
            lev->revRows[cursor[lev->entries[j].id]++] = r;
        }
    }
    CC_EDIT_FREE(cursor);

    return 0;
}


/*******************************************************************************
 * MeshCopy -- Deep-copies a control cage
 *
 */
static int cce__CopyArray(void **dst, const void *src, size_t bytes)
{
    if (bytes == 0) {
        *dst = NULL;

        return 0;
    }
    *dst = CC_EDIT_MALLOC(bytes);
    if (!*dst) {
        return -1;
    }
    CC_EDIT_MEMCPY(*dst, src, bytes);

    return 0;
}

static cc_Mesh *cce__MeshCopy(const cc_Mesh *src)
{
    cc_Mesh *dst = (cc_Mesh *)CC_EDIT_MALLOC(sizeof(*dst));

    if (!dst) {
        return NULL;
    }
    *dst = *src;
    dst->vertexToHalfedgeIDs = NULL;
    dst->edgeToHalfedgeIDs = NULL;
    dst->faceToHalfedgeIDs = NULL;
    dst->vertexPoints = NULL;
    dst->uvs = NULL;
    dst->halfedges = NULL;
    dst->creases = NULL;
    if (cce__CopyArray((void **)&dst->vertexToHalfedgeIDs,
                       src->vertexToHalfedgeIDs,
                       sizeof(int32_t) * (size_t)src->vertexCount)
    ||  cce__CopyArray((void **)&dst->edgeToHalfedgeIDs,
                       src->edgeToHalfedgeIDs,
                       sizeof(int32_t) * (size_t)src->edgeCount)
    ||  cce__CopyArray((void **)&dst->faceToHalfedgeIDs,
                       src->faceToHalfedgeIDs,
                       sizeof(int32_t) * (size_t)src->faceCount)
    ||  cce__CopyArray((void **)&dst->vertexPoints,
                       src->vertexPoints,
                       sizeof(cc_VertexPoint) * (size_t)src->vertexCount)
    ||  cce__CopyArray((void **)&dst->uvs,
                       src->uvs,
                       sizeof(cc_VertexUv) * (size_t)src->uvCount)
    ||  cce__CopyArray((void **)&dst->halfedges,
                       src->halfedges,
                       sizeof(cc_Halfedge) * (size_t)src->halfedgeCount)
    ||  cce__CopyArray((void **)&dst->creases,
                       src->creases,
                       sizeof(cc_Crease) * (size_t)src->edgeCount)) {
        ccm_Release(dst);

        return NULL;
    }

    return dst;
}


/*******************************************************************************
 * EdgeStencil -- Mirrors the upstream creased edge rule as a stencil
 *
 * out = lerp(smooth, sharp, edgeWeight) with
 * sharp = (v0 + v1) / 2 and smooth = (v0 + v1 + f0 + f1) / 4,
 * where v0, v1 are parent vertex rows and f0, f1 are same-level face rows.
 */
static int cce__EdgeStencil(cce__LevelBuild *lb,
                            cce__Stencil *sc,
                            int32_t v0,
                            int32_t v1,
                            int32_t f0,
                            int32_t f1,
                            float edgeWeight)
{
    const int32_t P = lb->parentCount;
    cce__Stencil *tmp1 = &sc[0];
    cce__Stencil *tmp2 = &sc[1];
    cce__Stencil *sharp = &sc[2];
    cce__Stencil *smooth = &sc[3];
    cce__Stencil *out = &sc[4];

    cce__StReset(tmp1);
    CCE__TRY(cce__StPush(tmp1, v0, 1.0f));
    CCE__TRY(cce__StPush(tmp1, v1, 1.0f));
    cce__StReset(tmp2);
    CCE__TRY(cce__StPush(tmp2, P + f0, 1.0f));
    CCE__TRY(cce__StPush(tmp2, P + f1, 1.0f));
    CCE__TRY(cce__StCopy(sharp, tmp1));
    cce__StScale(sharp, 0.5f);
    CCE__TRY(cce__StCopy(smooth, tmp1));
    CCE__TRY(cce__StAcc(smooth, tmp2, 1.0f));
    cce__StScale(smooth, 0.25f);
    CCE__TRY(cce__StLerp(out, smooth, sharp, edgeWeight));

    return cce__LbPushRow(lb, out);
}


/*******************************************************************************
 * Cage stencil builders -- Mirror the upstream "Cage" Gather rules
 *
 */
static int cce__CageVertexStencils(cc_EditPlan *plan,
                                   cce__LevelBuild *lb,
                                   cce__Stencil *sc)
{
    const cc_Mesh *cage = plan->cage;
    const int32_t vertexCount = ccm_VertexCount(cage);
    const int32_t faceCount = ccm_FaceCount(cage);
    const int32_t P = lb->parentCount;
    cce__Stencil *smoothPoint = &sc[0];
    cce__Stencil *creasePoint = &sc[1];
    cce__Stencil *out = &sc[2];

    for (int32_t vertexID = 0; vertexID < vertexCount; ++vertexID) {
        const int32_t halfedgeID = ccm_VertexToHalfedgeID(cage, vertexID);
        const int32_t edgeID = ccm_HalfedgeEdgeID(cage, halfedgeID);
        const int32_t prevID = ccm_HalfedgePrevID(cage, halfedgeID);
        const int32_t prevEdgeID = ccm_HalfedgeEdgeID(cage, prevID);
        const int32_t prevFaceID = ccm_HalfedgeFaceID(cage, prevID);
        const float thisS = ccm_HalfedgeSharpness(cage, halfedgeID);
        const float prevS = ccm_HalfedgeSharpness(cage, prevID);
        const float creaseWeight = cce__Signf(thisS);
        const float prevCreaseWeight = cce__Signf(prevS);
        float avgS = prevS;
        float creaseCount = prevCreaseWeight;
        float valence = 1.0f;
        int32_t forwardIterator;

        // smooth contrib
        cce__StReset(smoothPoint);
        CCE__TRY(cce__StPush(smoothPoint, P + prevFaceID, -1.0f));
        CCE__TRY(cce__StPush(smoothPoint, P + faceCount + prevEdgeID, +4.0f));

        // crease contrib
        cce__StReset(creasePoint);
        CCE__TRY(cce__StPush(creasePoint, P + faceCount + prevEdgeID, prevCreaseWeight));

        for (forwardIterator = ccm_HalfedgeTwinID(cage, prevID);
             forwardIterator >= 0 && forwardIterator != halfedgeID;
             forwardIterator = ccm_HalfedgeTwinID(cage, forwardIterator)) {
            const int32_t prevID2 = ccm_HalfedgePrevID(cage, forwardIterator);
            const int32_t prevEdgeID2 = ccm_HalfedgeEdgeID(cage, prevID2);
            const int32_t prevFaceID2 = ccm_HalfedgeFaceID(cage, prevID2);
            const float prevS2 = ccm_HalfedgeSharpness(cage, prevID2);
            const float prevCreaseWeight2 = cce__Signf(prevS2);

            // smooth contrib
            CCE__TRY(cce__StPush(smoothPoint, P + prevFaceID2, -1.0f));
            CCE__TRY(cce__StPush(smoothPoint, P + faceCount + prevEdgeID2, +4.0f));
            ++valence;

            // crease contrib
            CCE__TRY(cce__StPush(creasePoint, P + faceCount + prevEdgeID2, prevCreaseWeight2));
            avgS+= prevS2;
            creaseCount+= prevCreaseWeight2;

            // next vertex halfedge
            forwardIterator = prevID2;
        }

        // boundary corrections
        if (forwardIterator < 0) {
            CCE__TRY(cce__StPush(creasePoint, P + faceCount + edgeID, creaseWeight));
            creaseCount+= creaseWeight;
            ++valence;
        }

        // smooth point
        cce__StScale(smoothPoint, 1.0f / (valence * valence));
        CCE__TRY(cce__StPush(smoothPoint, vertexID, 1.0f - 3.0f / valence));

        // crease point
        cce__StScale(creasePoint, 0.25f);
        CCE__TRY(cce__StPush(creasePoint, vertexID, 0.5f));

        // proper vertex rule selection
        if (creaseCount <= 1.0f) {
            CCE__TRY(cce__StCopy(out, smoothPoint));
        } else if (creaseCount >= 3.0f || valence == 2.0f) {
            CCE__TRY(cce__StRef(out, vertexID));
        } else {
            const float t = cce__Satf(avgS * 0.5f);

            cce__StReset(out);
            CCE__TRY(cce__StPush(out, vertexID, 1.0f - t));
            CCE__TRY(cce__StAcc(out, creasePoint, t));
        }
        CCE__TRY(cce__LbPushRow(lb, out));
    }

    return 0;
}

static int cce__CageFaceStencils(cc_EditPlan *plan,
                                 cce__LevelBuild *lb,
                                 cce__Stencil *sc)
{
    const cc_Mesh *cage = plan->cage;
    const int32_t faceCount = ccm_FaceCount(cage);
    cce__Stencil *st = &sc[0];

    for (int32_t faceID = 0; faceID < faceCount; ++faceID) {
        const int32_t halfedgeID = ccm_FaceToHalfedgeID(cage, faceID);
        float faceVertexCount = 1.0f;

        cce__StReset(st);
        CCE__TRY(cce__StPush(st, ccm_HalfedgeVertexID(cage, halfedgeID), 1.0f));
        for (int32_t halfedgeIt = ccm_HalfedgeNextID(cage, halfedgeID);
             halfedgeIt != halfedgeID;
             halfedgeIt = ccm_HalfedgeNextID(cage, halfedgeIt)) {
            CCE__TRY(cce__StPush(st, ccm_HalfedgeVertexID(cage, halfedgeIt), 1.0f));
            ++faceVertexCount;
        }
        cce__StScale(st, 1.0f / faceVertexCount);
        CCE__TRY(cce__LbPushRow(lb, st));
    }

    return 0;
}

static int cce__CageEdgeStencils(cc_EditPlan *plan,
                                 cce__LevelBuild *lb,
                                 cce__Stencil *sc)
{
    const cc_Mesh *cage = plan->cage;
    const int32_t edgeCount = ccm_EdgeCount(cage);

    for (int32_t edgeID = 0; edgeID < edgeCount; ++edgeID) {
        const int32_t halfedgeID = ccm_EdgeToHalfedgeID(cage, edgeID);
        const int32_t twinID = ccm_HalfedgeTwinID(cage, halfedgeID);
        const int32_t nextID = ccm_HalfedgeNextID(cage, halfedgeID);
        const float edgeWeight = cce__Satf(ccm_CreaseSharpness(cage, edgeID));

        CCE__TRY(cce__EdgeStencil(lb, sc,
                                  ccm_HalfedgeVertexID(cage, halfedgeID),
                                  ccm_HalfedgeVertexID(cage, nextID),
                                  ccm_HalfedgeFaceID(cage, halfedgeID),
                                  ccm_HalfedgeFaceID(cage, cce__Max(0, twinID)),
                                  edgeWeight));
    }

    return 0;
}


/*******************************************************************************
 * Subd stencil builders -- Mirror the upstream semi-regular Gather rules
 *
 */
static int cce__SubdVertexStencils(cc_EditPlan *plan,
                                   cce__LevelBuild *lb,
                                   cce__Stencil *sc,
                                   int32_t depth)
{
    const cc_Subd *subd = plan->subd;
    const cc_Mesh *cage = plan->cage;
    const int32_t vertexCount = ccm_VertexCountAtDepth_Fast(cage, depth);
    const int32_t faceCount = ccm_FaceCountAtDepth_Fast(cage, depth);
    const int32_t P = lb->parentCount;
    cce__Stencil *smoothPoint = &sc[0];
    cce__Stencil *creasePoint = &sc[1];
    cce__Stencil *out = &sc[2];

    for (int32_t vertexID = 0; vertexID < vertexCount; ++vertexID) {
        const int32_t halfedgeID = ccs_VertexPointToHalfedgeID(subd, vertexID, depth);
        const int32_t edgeID = ccs_HalfedgeEdgeID(subd, halfedgeID, depth);
        const int32_t prevID = ccm_HalfedgePrevID_Quad(halfedgeID);
        const int32_t prevEdgeID = ccs_HalfedgeEdgeID(subd, prevID, depth);
        const int32_t prevFaceID = ccm_HalfedgeFaceID_Quad(prevID);
        const float thisS = ccs_HalfedgeSharpness(subd, halfedgeID, depth);
        const float prevS = ccs_HalfedgeSharpness(subd, prevID, depth);
        const float creaseWeight = cce__Signf(thisS);
        const float prevCreaseWeight = cce__Signf(prevS);
        float avgS = prevS;
        float creaseCount = prevCreaseWeight;
        float valence = 1.0f;
        int32_t forwardIterator, backwardIterator;

        // smooth contrib
        cce__StReset(smoothPoint);
        CCE__TRY(cce__StPush(smoothPoint, P + prevFaceID, -1.0f));
        CCE__TRY(cce__StPush(smoothPoint, P + faceCount + prevEdgeID, +4.0f));

        // crease contrib
        cce__StReset(creasePoint);
        CCE__TRY(cce__StPush(creasePoint, P + faceCount + prevEdgeID, prevCreaseWeight));

        for (forwardIterator = ccs_HalfedgeTwinID(subd, prevID, depth);
             forwardIterator >= 0 && forwardIterator != halfedgeID;
             forwardIterator = ccs_HalfedgeTwinID(subd, forwardIterator, depth)) {
            const int32_t prevID2 = ccm_HalfedgePrevID_Quad(forwardIterator);
            const int32_t prevEdgeID2 = ccs_HalfedgeEdgeID(subd, prevID2, depth);
            const int32_t prevFaceID2 = ccm_HalfedgeFaceID_Quad(prevID2);
            const float prevS2 = ccs_HalfedgeSharpness(subd, prevID2, depth);
            const float prevCreaseWeight2 = cce__Signf(prevS2);

            // smooth contrib
            CCE__TRY(cce__StPush(smoothPoint, P + prevFaceID2, -1.0f));
            CCE__TRY(cce__StPush(smoothPoint, P + faceCount + prevEdgeID2, +4.0f));
            ++valence;

            // crease contrib
            CCE__TRY(cce__StPush(creasePoint, P + faceCount + prevEdgeID2, prevCreaseWeight2));
            avgS+= prevS2;
            creaseCount+= prevCreaseWeight2;

            // next vertex halfedge
            forwardIterator = prevID2;
        }

        for (backwardIterator = ccs_HalfedgeTwinID(subd, halfedgeID, depth);
             forwardIterator < 0 && backwardIterator >= 0 && backwardIterator != halfedgeID;
             backwardIterator = ccs_HalfedgeTwinID(subd, backwardIterator, depth)) {
            const int32_t nextID = ccm_HalfedgeNextID_Quad(backwardIterator);
            const int32_t nextEdgeID = ccs_HalfedgeEdgeID(subd, nextID, depth);
            const int32_t nextFaceID = ccm_HalfedgeFaceID_Quad(nextID);
            const float nextS = ccs_HalfedgeSharpness(subd, nextID, depth);
            const float nextCreaseWeight = cce__Signf(nextS);

            // smooth contrib
            CCE__TRY(cce__StPush(smoothPoint, P + nextFaceID, -1.0f));
            CCE__TRY(cce__StPush(smoothPoint, P + faceCount + nextEdgeID, +4.0f));
            ++valence;

            // crease contrib
            CCE__TRY(cce__StPush(creasePoint, P + faceCount + nextEdgeID, nextCreaseWeight));
            avgS+= nextS;
            creaseCount+= nextCreaseWeight;

            // next vertex halfedge
            backwardIterator = nextID;
        }

        // boundary corrections
        if (forwardIterator < 0) {
            CCE__TRY(cce__StPush(creasePoint, P + faceCount + edgeID, creaseWeight));
            creaseCount+= creaseWeight;
            ++valence;
        }

        // smooth point
        cce__StScale(smoothPoint, 1.0f / (valence * valence));
        CCE__TRY(cce__StPush(smoothPoint, vertexID, 1.0f - 3.0f / valence));

        // crease point
        cce__StScale(creasePoint, 0.5f / creaseCount);
        CCE__TRY(cce__StPush(creasePoint, vertexID, 0.5f));

        // proper vertex rule selection
        if (creaseCount <= 1.0f) {
            CCE__TRY(cce__StCopy(out, smoothPoint));
        } else if (creaseCount >= 3.0f || valence == 2.0f) {
            CCE__TRY(cce__StRef(out, vertexID));
        } else {
            const float t = cce__Satf(avgS * 0.5f);

            cce__StReset(out);
            CCE__TRY(cce__StPush(out, vertexID, 1.0f - t));
            CCE__TRY(cce__StAcc(out, creasePoint, t));
        }
        CCE__TRY(cce__LbPushRow(lb, out));
    }

    return 0;
}

static int cce__SubdFaceStencils(cc_EditPlan *plan,
                                 cce__LevelBuild *lb,
                                 cce__Stencil *sc,
                                 int32_t depth)
{
    const cc_Subd *subd = plan->subd;
    const cc_Mesh *cage = plan->cage;
    const int32_t faceCount = ccm_FaceCountAtDepth_Fast(cage, depth);
    cce__Stencil *st = &sc[0];

    for (int32_t faceID = 0; faceID < faceCount; ++faceID) {
        const int32_t halfedgeID = ccm_FaceToHalfedgeID_Quad(faceID);

        cce__StReset(st);
        for (int32_t k = 0; k < 4; ++k) {
            CCE__TRY(cce__StPush(st,
                                 ccs_HalfedgeVertexID(subd, halfedgeID + k, depth),
                                 1.0f));
        }
        cce__StScale(st, 0.25f);
        CCE__TRY(cce__LbPushRow(lb, st));
    }

    return 0;
}

static int cce__SubdEdgeStencils(cc_EditPlan *plan,
                                 cce__LevelBuild *lb,
                                 cce__Stencil *sc,
                                 int32_t depth)
{
    const cc_Subd *subd = plan->subd;
    const cc_Mesh *cage = plan->cage;
    const int32_t edgeCount = ccm_EdgeCountAtDepth_Fast(cage, depth);

    for (int32_t edgeID = 0; edgeID < edgeCount; ++edgeID) {
        const int32_t halfedgeID = ccs_EdgeToHalfedgeID(subd, edgeID, depth);
        const int32_t twinID = ccs_HalfedgeTwinID(subd, halfedgeID, depth);
        const int32_t nextID = ccm_HalfedgeNextID_Quad(halfedgeID);
        const float edgeWeight = cce__Satf(ccs_CreaseSharpness(subd, edgeID, depth));

        CCE__TRY(cce__EdgeStencil(lb, sc,
                                  ccs_HalfedgeVertexID(subd, halfedgeID, depth),
                                  ccs_HalfedgeVertexID(subd, nextID, depth),
                                  ccm_HalfedgeFaceID_Quad(halfedgeID),
                                  ccm_HalfedgeFaceID_Quad(cce__Max(0, twinID)),
                                  edgeWeight));
    }

    return 0;
}


/*******************************************************************************
 * BuildLevel -- Builds the stencils and the reverse index of one level
 *
 */
static int cce__BuildLevel(cc_EditPlan *plan, int32_t depth)
{
    const cc_Mesh *cage = plan->cage;
    cce__Level *lev = &plan->levels[depth - 1];
    cce__LevelBuild lb;
    cce__Stencil sc[6];
    int rc;

    for (int32_t i = 0; i < 6; ++i) {
        cce__StInit(&sc[i]);
    }
    rc = cce__LbInit(&lb,
                     ccm_VertexCountAtDepth(cage, depth - 1),
                     ccm_VertexCountAtDepth(cage, depth));
    if (rc == 0) {
        if (depth == 1) {
            rc = cce__CageVertexStencils(plan, &lb, sc)
              || cce__CageFaceStencils(plan, &lb, sc)
              || cce__CageEdgeStencils(plan, &lb, sc);
        } else {
            rc = cce__SubdVertexStencils(plan, &lb, sc, depth - 1)
              || cce__SubdFaceStencils(plan, &lb, sc, depth - 1)
              || cce__SubdEdgeStencils(plan, &lb, sc, depth - 1);
        }
    }
    if (rc == 0) {
        lev->parentCount = lb.parentCount;
        lev->rowCount = lb.rowCount;
        lev->offsets = lb.offsets;
        lb.offsets = NULL;
        lev->entries = lb.entries;
        lb.entries = NULL;
        rc = cce__LevelBuildReverse(lev);
    }
    cce__LbRelease(&lb);
    for (int32_t i = 0; i < 6; ++i) {
        cce__StRelease(&sc[i]);
    }

    return rc;
}


/*******************************************************************************
 * Create -- Creates an edit plan
 *
 * The control cage (positions, uvs, halfedges, mappings and creases) is
 * deep-copied and the full initial subdivision is computed, so the source
 * cage may be edited or released as soon as this routine returns.
 */
CCEDEF cc_EditPlan *ccs_EditPlanCreate(const cc_Mesh *cage, int32_t depth)
{
    cc_EditPlan *plan;
    int32_t bufCapacity;
    int32_t cumVertexCount;

    if (!cage || depth < 1 || depth > 5) {
        return NULL;
    }
    if (cage->vertexCount <= 0 || cage->halfedgeCount <= 0
    ||  cage->edgeCount <= 0 || cage->faceCount <= 0) {
        return NULL;
    }
    if (!cage->vertexPoints || !cage->halfedges || !cage->creases
    ||  !cage->vertexToHalfedgeIDs || !cage->edgeToHalfedgeIDs
    ||  !cage->faceToHalfedgeIDs) {
        return NULL;
    }
    if (cage->uvCount < 0 || (cage->uvCount > 0 && !cage->uvs)) {
        return NULL;
    }

    plan = (cc_EditPlan *)CC_EDIT_MALLOC(sizeof(*plan));
    if (!plan) {
        return NULL;
    }
    CC_EDIT_MEMSET(plan, 0, sizeof(*plan));
    plan->depth = depth;

    plan->cage = cce__MeshCopy(cage);
    if (!plan->cage) {
        goto fail;
    }

    plan->subd = ccs_Create(plan->cage, depth);
    if (!plan->subd
    ||  !plan->subd->halfedges
    ||  !plan->subd->creases
    ||  !plan->subd->vertexPoints) {
        goto fail;
    }
    ccs_Refine_Gather(plan->subd);

    cumVertexCount = ccs_CumulativeVertexCount(plan->subd);
    plan->rowGen = (uint32_t *)CC_EDIT_MALLOC(sizeof(uint32_t) * (size_t)cumVertexCount);
    plan->cageGen = (uint32_t *)CC_EDIT_MALLOC(sizeof(uint32_t) * (size_t)cage->vertexCount);
    if (!plan->rowGen || !plan->cageGen) {
        goto fail;
    }
    CC_EDIT_MEMSET(plan->rowGen, 0, sizeof(uint32_t) * (size_t)cumVertexCount);
    CC_EDIT_MEMSET(plan->cageGen, 0, sizeof(uint32_t) * (size_t)cage->vertexCount);
    plan->generation = 0;

    bufCapacity = cage->vertexCount;
    for (int32_t d = 1; d <= depth; ++d) {
        bufCapacity = cce__Max(bufCapacity, ccm_VertexCountAtDepth(cage, d));
    }
    plan->bufA = (int32_t *)CC_EDIT_MALLOC(sizeof(int32_t) * (size_t)bufCapacity);
    plan->bufB = (int32_t *)CC_EDIT_MALLOC(sizeof(int32_t) * (size_t)bufCapacity);
    if (!plan->bufA || !plan->bufB) {
        goto fail;
    }

    for (int32_t d = 1; d <= depth; ++d) {
        if (cce__BuildLevel(plan, d)) {
            goto fail;
        }
    }

    return plan;

fail:
    ccs_EditPlanRelease(plan);

    return NULL;
}


/*******************************************************************************
 * Release -- Releases memory used by an edit plan
 *
 */
CCEDEF void ccs_EditPlanRelease(cc_EditPlan *plan)
{
    if (!plan) {
        return;
    }
    for (int32_t i = 0; i < 5; ++i) {
        CC_EDIT_FREE(plan->levels[i].offsets);
        CC_EDIT_FREE(plan->levels[i].entries);
        CC_EDIT_FREE(plan->levels[i].revOffsets);
        CC_EDIT_FREE(plan->levels[i].revRows);
    }
    if (plan->subd) {
        ccs_Release(plan->subd);
    }
    if (plan->cage) {
        ccm_Release(plan->cage);
    }
    CC_EDIT_FREE(plan->rowGen);
    CC_EDIT_FREE(plan->cageGen);
    CC_EDIT_FREE(plan->bufA);
    CC_EDIT_FREE(plan->bufB);
    CC_EDIT_FREE(plan);
}


/*******************************************************************************
 * Subd -- Returns the plan-owned read-only subdivision result
 *
 */
CCEDEF const cc_Subd *ccs_EditPlanSubd(const cc_EditPlan *plan)
{
    return plan ? plan->subd : NULL;
}


/*******************************************************************************
 * Update -- Applies a sparse batch of control cage position edits
 *
 * The batch is fully validated before anything is mutated, so a rejected
 * update leaves the cage copy and every subdivision level untouched. Only
 * the rows that actually depend on the edited vertices are visited and
 * re-evaluated, level by level (face rows, then edge rows, then vertex
 * rows), following the stencils built at plan creation.
 */
CCEDEF int ccs_EditPlanUpdate(cc_EditPlan *plan,
                              const int32_t *ids,
                              const cc_VertexPoint *positions,
                              size_t count,
                              cc_EditStats *stats)
{
    int32_t *parentList, *marked;
    int32_t parentN, vertexCount;

    if (stats) {
        CC_EDIT_MEMSET(stats, 0, sizeof(*stats));
    }
    if (!plan) {
        return 1;
    }
    if (count > 0 && (!ids || !positions)) {
        return 1;
    }
    vertexCount = plan->cage->vertexCount;

    // bump the scratch generation (with wraparound recovery)
    ++plan->generation;
    if (plan->generation == 0) {
        CC_EDIT_MEMSET(plan->rowGen,
                       0,
                       sizeof(uint32_t) * (size_t)ccs_CumulativeVertexCount(plan->subd));
        CC_EDIT_MEMSET(plan->cageGen,
                       0,
                       sizeof(uint32_t) * (size_t)vertexCount);
        plan->generation = 1;
    }

    // validate the whole batch before mutating anything
    for (size_t i = 0; i < count; ++i) {
        const int32_t id = ids[i];
        const float *p = positions[i].array;

        if (id < 0 || id >= vertexCount) {
            return 1;
        }
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
            return 1;
        }
        if (plan->cageGen[id] == plan->generation) {
            return 1; // duplicate id
        }
        plan->cageGen[id] = plan->generation;
    }

    // collect the vertices whose position actually changes
    parentN = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t id = ids[i];
        const cc_VertexPoint *cur = &plan->cage->vertexPoints[id];
        const cc_VertexPoint *nxt = &positions[i];

        if (cur->x != nxt->x || cur->y != nxt->y || cur->z != nxt->z) {
            plan->bufA[parentN++] = (int32_t)i;
        }
    }
    if (parentN == 0) {
        return 0; // no geometric work
    }

    // commit the new positions to the private cage copy
    for (int32_t k = 0; k < parentN; ++k) {
        const int32_t i = plan->bufA[k];

        plan->cage->vertexPoints[ids[i]] = positions[i];
        plan->bufA[k] = ids[i];
    }

    // propagate and re-evaluate, level by level
    parentList = plan->bufA;
    marked = plan->bufB;
    for (int32_t depth = 1; depth <= plan->depth; ++depth) {
        const cce__Level *lev = &plan->levels[depth - 1];
        const int32_t P = lev->parentCount;
        const int32_t F = ccm_FaceCountAtDepth(plan->cage, depth - 1);
        const int32_t stride = ccs_CumulativeVertexCountAtDepth(plan->cage, depth - 1);
        cc_VertexPoint *rows = &plan->subd->vertexPoints[stride];
        const cc_VertexPoint *parentPoints = (depth == 1)
            ? plan->cage->vertexPoints
            : &plan->subd->vertexPoints[ccs_CumulativeVertexCountAtDepth(plan->cage, depth - 2)];
        int32_t markedN = 0;
        size_t visited = 0;

        // candidate rows reached from the dirty parent rows
        for (int32_t k = 0; k < parentN; ++k) {
            const int32_t node = parentList[k];

            for (size_t j = lev->revOffsets[node]; j < lev->revOffsets[node + 1]; ++j) {
                const int32_t r = lev->revRows[j];

                ++visited;
                if (plan->rowGen[stride + r] != plan->generation) {
                    plan->rowGen[stride + r] = plan->generation;
                    marked[markedN++] = r;
                }
            }
        }
        // candidate rows reached from dirty same-level rows
        for (int32_t k = 0; k < markedN; ++k) {
            const int32_t node = marked[k];

            if (node < P) {
                continue; // vertex rows are never referenced within a level
            }
            for (size_t j = lev->revOffsets[node]; j < lev->revOffsets[node + 1]; ++j) {
                const int32_t r = lev->revRows[j];

                ++visited;
                if (plan->rowGen[stride + r] != plan->generation) {
                    plan->rowGen[stride + r] = plan->generation;
                    marked[markedN++] = r;
                }
            }
        }

        // re-evaluate: face rows, then edge rows, then vertex rows
        for (int32_t pass = 0; pass < 3; ++pass) {
            for (int32_t k = 0; k < markedN; ++k) {
                const int32_t r = marked[k];
                const bool isFace = (r >= P && r < P + F);
                const bool isEdge = (r >= P + F);
                float acc[3] = {0.0f, 0.0f, 0.0f};

                if ((pass == 0 && !isFace)
                ||  (pass == 1 && !isEdge)
                ||  (pass == 2 && (isFace || isEdge))) {
                    continue;
                }
                for (size_t j = lev->offsets[r]; j < lev->offsets[r + 1]; ++j) {
                    const int32_t id = lev->entries[j].id;
                    const float w = lev->entries[j].w;
                    const cc_VertexPoint *src = id < P
                        ? &parentPoints[id]
                        : &rows[id];

                    acc[0]+= w * src->array[0];
                    acc[1]+= w * src->array[1];
                    acc[2]+= w * src->array[2];
                }
                rows[r].array[0] = acc[0];
                rows[r].array[1] = acc[1];
                rows[r].array[2] = acc[2];
            }
        }

        if (stats) {
            stats->evaluated_rows[depth - 1] = (size_t)markedN;
            stats->visited_rows[depth - 1] = visited;
        }

        // dirty rows of this level are the dirty parents of the next one
        {
            int32_t *tmp = parentList;

            parentList = marked;
            marked = tmp;
            parentN = markedN;
        }
    }

    return 0;
}


#undef CC_EDIT_ASSERT
#undef CC_EDIT_MALLOC
#undef CC_EDIT_MEMCPY
#undef CC_EDIT_MEMSET
#undef CC_EDIT_FREE

#endif // CC_EDIT_IMPLEMENTATION
