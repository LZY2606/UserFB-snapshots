/******************************************************************************
 *
 * Project:  Shapelib
 * Purpose:  Unit tests for SHPBuildPolygonComponents().
 *
 * Registered through devcheck/check.h in four groups: hierarchy,
 * predicates, provenance and files. The file has no main() of its own;
 * devcheck/target-main.c drives the registered unit_cases.
 *
 ******************************************************************************
 * SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
 ******************************************************************************
 */

#include "check.h"
#include "shapefil.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/************************************************************************/
/*                              Helpers                                 */
/************************************************************************/

static SHPObject *pc_make(int nType, int nShapeId, int nParts,
                          const int *panPartStart, int nVertices,
                          const double *padfX, const double *padfY,
                          const double *padfZ, const double *padfM)
{
    return SHPCreateObject(nType, nShapeId, nParts, panPartStart, NULL,
                           nVertices, padfX, padfY, padfZ, padfM);
}

/* Signed doubled area of one part of an object. */
static double pc_ring_area(const SHPObject *psObject, int iPart)
{
    const int nStart = psObject->panPartStart[iPart];
    const int nCount =
        (iPart == psObject->nParts - 1)
            ? psObject->nVertices - nStart
            : psObject->panPartStart[iPart + 1] - nStart;
    double dfSum = 0.0;
    for (int i = 0; i < nCount - 1; i++)
    {
        const double x0 = psObject->padfX[nStart + i];
        const double y0 = psObject->padfY[nStart + i];
        const double x1 = psObject->padfX[nStart + i + 1];
        const double y1 = psObject->padfY[nStart + i + 1];
        dfSum += x0 * y1 - y0 * x1;
    }
    return dfSum;
}

/* Canonical ring description: arrays already include the closing vertex. */
typedef struct
{
    int nLen;
    const double *padfX;
    const double *padfY;
} PCCanonRing;

/*
 * Assemble a polygon object from canonical rings, optionally permuting the
 * source parts (anPerm[p] = canonical id of source part p), rotating the
 * start vertex of every ring and/or reversing its direction. Used to prove
 * that hierarchy results depend on ring identity, not storage order,
 * starting vertex or input orientation.
 */
static SHPObject *pc_assemble(int nType, int nRings,
                              const PCCanonRing *pasRings, const int *anPerm,
                              const int *anRot, const int *anRev, int nShapeId)
{
    int nVertices = 0;
    for (int i = 0; i < nRings; i++)
        nVertices += pasRings[i].nLen;

    double *padfX = calloc((size_t)nVertices, sizeof(double));
    double *padfY = calloc((size_t)nVertices, sizeof(double));
    int *panPartStart = calloc((size_t)nRings, sizeof(int));
    if (!padfX || !padfY || !panPartStart)
    {
        free(padfX);
        free(padfY);
        free(panPartStart);
        return NULL;
    }

    int nCursor = 0;
    for (int p = 0; p < nRings; p++)
    {
        const PCCanonRing *psR = &pasRings[anPerm[p]];
        const int nDistinct = psR->nLen - 1;
        panPartStart[p] = nCursor;
        for (int k = 0; k < nDistinct; k++)
        {
            int s = (anRot[p] + k) % nDistinct;
            if (anRev[p])
                s = (anRot[p] - k + nDistinct * 2) % nDistinct;
            padfX[nCursor + k] = psR->padfX[s];
            padfY[nCursor + k] = psR->padfY[s];
        }
        padfX[nCursor + nDistinct] = padfX[nCursor];
        padfY[nCursor + nDistinct] = padfY[nCursor];
        nCursor += psR->nLen;
    }

    SHPObject *psObject =
        pc_make(nType, nShapeId, nRings, panPartStart, nVertices, padfX,
                padfY, NULL, NULL);
    free(padfX);
    free(padfY);
    free(panPartStart);
    return psObject;
}

/* Check the hierarchy of an assembled object against canonical identities. */
static void pc_check_hierarchy(unit_context *c, SHPObject *psObject,
                               int nRings, const int *anPerm,
                               const int *anCanonParent,
                               const int *anCanonDepth, int nExpComponents)
{
    SHPPolygonComponents *psRes = NULL;
    char *pszError = NULL;
    CHECK(c, SHPBuildPolygonComponents(psObject, &psRes, &pszError) == 1);
    CHECK(c, pszError == NULL);
    CHECK(c, psRes != NULL);
    CHECK(c, psRes->nRings == nRings);

    int anPos[32];
    CHECK(c, nRings <= 32);
    for (int p = 0; p < nRings; p++)
        anPos[anPerm[p]] = p;

    for (int p = 0; p < nRings; p++)
    {
        const int nCanon = anPerm[p];
        if (anCanonParent[nCanon] < 0)
        {
            CHECK(c, psRes->panParent[p] == -1);
            CHECK(c, psRes->panDepth[p] == 0);
        }
        else
        {
            CHECK(c, psRes->panParent[p] == anPos[anCanonParent[nCanon]]);
            CHECK(c, psRes->panDepth[p] == anCanonDepth[nCanon]);
        }
    }

    CHECK(c, psRes->nComponents == nExpComponents);

    /* Components must be ordered by the source part index of their outer  */
    /* ring. Build the sorted root source parts and compare.               */
    int anExpOuterPart[32];
    int nRoots = 0;
    for (int nCanon = 0; nCanon < nRings; nCanon++)
    {
        if (anCanonDepth[nCanon] % 2 == 0)
            anExpOuterPart[nRoots++] = anPos[nCanon];
    }
    CHECK(c, nRoots == nExpComponents);
    for (int i = 1; i < nRoots; i++)
    {
        for (int j = i; j > 0 && anExpOuterPart[j - 1] > anExpOuterPart[j];
             j--)
        {
            const int nTmp = anExpOuterPart[j - 1];
            anExpOuterPart[j - 1] = anExpOuterPart[j];
            anExpOuterPart[j] = nTmp;
        }
    }
    for (int i = 0; i < nExpComponents; i++)
    {
        CHECK(c, psRes->pasComponents[i].nOuterSrcPart ==
                anExpOuterPart[i]);
        CHECK(c, psRes->pasComponents[i].psObject != NULL);
        CHECK(c, psRes->pasComponents[i].psObject->nShapeId ==
                psObject->nShapeId);
        /* Outer ring is always part 0 of the output object. */
        CHECK(c, psRes->pasComponents[i].pasRings[0].nSrcPart ==
                anExpOuterPart[i]);
    }

    SHPDestroyPolygonComponents(psRes);
}

/* Locate the component whose outer ring is the given source part. */
static const SHPPolygonComponent *
pc_find_component(const SHPPolygonComponents *psRes, int nOuterSrcPart)
{
    for (int i = 0; i < psRes->nComponents; i++)
    {
        if (psRes->pasComponents[i].nOuterSrcPart == nOuterSrcPart)
            return &psRes->pasComponents[i];
    }
    return NULL;
}

static void pc_expect_failure(unit_context *c, SHPObject *psObject)
{
    SHPPolygonComponents *psRes = (SHPPolygonComponents *)(intptr_t)0x1;
    char *pszError = NULL;
    CHECK(c, SHPBuildPolygonComponents(psObject, &psRes, &pszError) == 0);
    CHECK(c, psRes == NULL);
    CHECK(c, pszError != NULL);
    free(pszError);
}

/************************************************************************/
/*                           hierarchy group                            */
/************************************************************************/

/* Outer square, hole, island in the hole. */
static void test_unordered_rings(unit_context *c)
{
    static const double r0x[] = {0, 20, 20, 0, 0};
    static const double r0y[] = {0, 0, 20, 20, 0};
    static const double r1x[] = {4, 16, 16, 4, 4};
    static const double r1y[] = {4, 4, 16, 16, 4};
    static const double r2x[] = {8, 12, 12, 8, 8};
    static const double r2y[] = {8, 8, 12, 12, 8};
    const PCCanonRing asRings[] = {{5, r0x, r0y}, {5, r1x, r1y},
                                   {5, r2x, r2y}};
    const int anCanonParent[] = {-1, 0, 1};
    const int anCanonDepth[] = {0, 1, 2};

    /* Plain order. */
    {
        const int anPerm[] = {0, 1, 2};
        const int anRot[] = {0, 0, 0};
        const int anRev[] = {0, 0, 0};
        SHPObject *psObject = pc_assemble(SHPT_POLYGON, 3, asRings, anPerm,
                                          anRot, anRev, 4);
        CHECK(c, psObject != NULL);
        pc_check_hierarchy(c, psObject, 3, anPerm, anCanonParent,
                           anCanonDepth, 2);
        SHPDestroyObject(psObject);
    }

    /* Shuffled parts, rotated ring starts, mixed directions. */
    {
        const int anPerm[] = {2, 0, 1}; /* island, outer, hole */
        const int anRot[] = {1, 2, 3};
        const int anRev[] = {1, 0, 1};
        SHPObject *psObject = pc_assemble(SHPT_POLYGON, 3, asRings, anPerm,
                                          anRot, anRev, 4);
        CHECK(c, psObject != NULL);
        pc_check_hierarchy(c, psObject, 3, anPerm, anCanonParent,
                           anCanonDepth, 2);

        /* The component of the outer ring carries the hole as its only    */
        /* child ring, and the island forms a component of its own.        */
        SHPPolygonComponents *psRes = NULL;
        CHECK(c, SHPBuildPolygonComponents(psObject, &psRes, NULL) == 1);
        int anPos[3];
        for (int p = 0; p < 3; p++)
            anPos[anPerm[p]] = p;
        const SHPPolygonComponent *psOuter =
            pc_find_component(psRes, anPos[0]);
        const SHPPolygonComponent *psIsland =
            pc_find_component(psRes, anPos[2]);
        CHECK(c, psOuter != NULL && psIsland != NULL);
        CHECK(c, psOuter->nRings == 2);
        CHECK(c, psOuter->pasRings[1].nSrcPart == anPos[1]);
        CHECK(c, psIsland->nRings == 1);
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(psObject);
    }

    /* Every ring reversed: identity-based hierarchy must be unchanged. */
    {
        const int anPerm[] = {1, 2, 0};
        const int anRot[] = {0, 0, 0};
        const int anRev[] = {1, 1, 1};
        SHPObject *psObject = pc_assemble(SHPT_POLYGON, 3, asRings, anPerm,
                                          anRot, anRev, 4);
        CHECK(c, psObject != NULL);
        pc_check_hierarchy(c, psObject, 3, anPerm, anCanonParent,
                           anCanonDepth, 2);
        SHPDestroyObject(psObject);
    }
}

/* Two disjoint outer rings, each with its own hole. */
static void test_disjoint_parents(unit_context *c)
{
    static const double a0x[] = {0, 10, 10, 0, 0};
    static const double a0y[] = {0, 0, 10, 10, 0};
    static const double a1x[] = {2, 4, 4, 2, 2};
    static const double a1y[] = {2, 2, 4, 4, 2};
    static const double b0x[] = {30, 40, 40, 30, 30};
    static const double b0y[] = {0, 0, 10, 10, 0};
    static const double b1x[] = {32, 34, 34, 32, 32};
    static const double b1y[] = {2, 2, 4, 4, 2};
    const PCCanonRing asRings[] = {{5, a0x, a0y}, {5, a1x, a1y},
                                   {5, b0x, b0y}, {5, b1x, b1y}};
    const int anCanonParent[] = {-1, 0, -1, 2};
    const int anCanonDepth[] = {0, 1, 0, 1};

    const int anPerm[] = {3, 1, 0, 2};
    const int anRot[] = {2, 1, 3, 0};
    const int anRev[] = {0, 1, 1, 0};
    SHPObject *psObject = pc_assemble(SHPT_POLYGON, 4, asRings, anPerm,
                                      anRot, anRev, 9);
    CHECK(c, psObject != NULL);
    pc_check_hierarchy(c, psObject, 4, anPerm, anCanonParent, anCanonDepth,
                       2);

    /* Each component carries its own hole; holes follow source part      */
    /* order within the component. Components follow outer source parts.  */
    SHPPolygonComponents *psRes = NULL;
    CHECK(c, SHPBuildPolygonComponents(psObject, &psRes, NULL) == 1);
    int anPos[4];
    for (int p = 0; p < 4; p++)
        anPos[anPerm[p]] = p;
    const SHPPolygonComponent *psA = pc_find_component(psRes, anPos[0]);
    const SHPPolygonComponent *psB = pc_find_component(psRes, anPos[2]);
    CHECK(c, psA != NULL && psB != NULL);
    CHECK(c, psA->nRings == 2);
    CHECK(c, psA->pasRings[1].nSrcPart == anPos[1]);
    CHECK(c, psB->nRings == 2);
    CHECK(c, psB->pasRings[1].nSrcPart == anPos[3]);
    CHECK(c, psRes->pasComponents[0].nOuterSrcPart <
                psRes->pasComponents[1].nOuterSrcPart);
    SHPDestroyPolygonComponents(psRes);
    SHPDestroyObject(psObject);
}

/* A ring inside the bounding box of a concave ring but outside the ring */
/* itself (the U-shape notch) must not be classified as a hole.          */
static void test_concave_bbox_trap(unit_context *c)
{
    /* U-shape, notch open at the top: x in [6,14], y in [6,20]. */
    static const double ux[] = {0, 20, 20, 14, 14, 6, 6, 0, 0};
    static const double uy[] = {0, 0, 20, 20, 6, 6, 20, 20, 0};
    /* Square floating in the notch: inside the bbox, outside the polygon. */
    static const double sx[] = {8, 12, 12, 8, 8};
    static const double sy[] = {12, 12, 18, 18, 12};
    const PCCanonRing asRings[] = {{9, ux, uy}, {5, sx, sy}};
    const int anCanonParent[] = {-1, -1};
    const int anCanonDepth[] = {0, 0};

    const int anPerm[] = {1, 0};
    const int anRot[] = {0, 2};
    const int anRev[] = {1, 0};
    SHPObject *psObject = pc_assemble(SHPT_POLYGON, 2, asRings, anPerm,
                                      anRot, anRev, 0);
    CHECK(c, psObject != NULL);
    pc_check_hierarchy(c, psObject, 2, anPerm, anCanonParent, anCanonDepth,
                       2);
    SHPDestroyObject(psObject);
}

/************************************************************************/
/*                          predicates group                            */
/************************************************************************/

static void test_ring_validity(unit_context *c)
{
    /* Not explicitly closed. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 4, 0, 0.5};
        const double y[] = {0, 0, 4, 4, 0.5};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Fewer than three distinct XY points. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 4, 0, 0};
        const double y[] = {0, 0, 0, 0, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Zero-length edge. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 4, 4, 0, 0};
        const double y[] = {0, 0, 0, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 6, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Zero area (bowtie). */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 4, 0, 0};
        const double y[] = {0, 4, 0, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Collinear back-fold (spike) with distinct, non-degenerate points. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 2, 2, 0, 0};
        const double y[] = {0, 0, 0, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 6, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Figure-eight style ring touching itself at a non-adjacent vertex. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, 4, 8, 8, 4, 4, 0, 0};
        const double y[] = {0, 0, 4, 4, 8, 8, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 9, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }
}

static void test_ring_contacts(unit_context *c)
{
    /* Two rings sharing a single vertex. */
    {
        const int anParts[] = {0, 5};
        const double x[] = {0, 4, 4, 0, 0, 4, 8, 8, 4, 4};
        const double y[] = {0, 0, 4, 4, 0, 4, 4, 8, 8, 4};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anParts, 10, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Two rings overlapping along a collinear edge segment. */
    {
        const int anParts[] = {0, 5};
        const double x[] = {0, 6, 6, 0, 0, 4, 10, 10, 4, 4};
        const double y[] = {0, 0, 4, 4, 0, 0, 0, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anParts, 10, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* A diamond crossing the edges of a square. */
    {
        const int anParts[] = {0, 5};
        const double x[] = {0, 10, 10, 0, 0, 5, 12, 5, -2, 5};
        const double y[] = {0, 0, 10, 10, 0, -2, 5, 12, 5, -2};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anParts, 10, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Narrow but exact gap: the rings must not be merged by an epsilon. */
    {
        const double dGap = ldexp(1.0, -20); /* 2^-20, well above 2^-30 */
        const int anParts[] = {0, 5};
        double x[10], y[10];
        const double ax[] = {0, 4, 4, 0, 0};
        const double ay[] = {0, 0, 4, 4, 0};
        for (int i = 0; i < 5; i++)
        {
            x[i] = ax[i];
            y[i] = ay[i];
            x[5 + i] = 4.0 + dGap + ax[i];
            y[5 + i] = ay[i];
        }
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anParts, 10, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        SHPPolygonComponents *psRes = NULL;
        CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
        CHECK(c, psRes->nComponents == 2);
        CHECK(c, psRes->panParent[0] == -1);
        CHECK(c, psRes->panParent[1] == -1);
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(o);

        /* Legal translation and uniform scaling must keep the gap open. */
        for (int i = 0; i < 10; i++)
        {
            x[i] = x[i] * 0.5 + 100.0;
            y[i] = y[i] * 0.5 + 100.0;
        }
        o = pc_make(SHPT_POLYGON, -1, 2, anParts, 10, x, y, NULL, NULL);
        CHECK(c, o != NULL);
        CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
        CHECK(c, psRes->nComponents == 2);
        CHECK(c, psRes->panParent[1] == -1);
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(o);
    }

    /* A very thin but valid triangle is accepted. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 10, 10, 0};
        const double y[] = {0, 0, ldexp(1.0, -20), 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 4, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        SHPPolygonComponents *psRes = NULL;
        CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
        CHECK(c, psRes->nComponents == 1);
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(o);
    }
}

static void test_ranges_and_types(unit_context *c)
{
    /* Non-finite XY. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, NAN, 0, 0};
        const double y[] = {0, 0, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }
    {
        const int anParts[] = {0};
        const double x[] = {0, 4, INFINITY, 0, 0};
        const double y[] = {0, 0, 4, 4, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Coordinates below 2^-30 (non-zero) and above 2^30 are rejected. */
    {
        const int anParts[] = {0};
        const double dSmall = ldexp(1.0, -31);
        const double x[] = {0, 1, 1, 0, 0};
        const double y[] = {0, 0, dSmall, dSmall, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }
    {
        const int anParts[] = {0};
        const double dBig = ldexp(1.0, 31);
        const double x[] = {0, 1, 1, 0, 0};
        const double y[] = {0, 0, dBig, dBig, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Zero coordinates are explicitly allowed. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 1, 1, 0, 0};
        const double y[] = {0, 0, 1, 1, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 1, anParts, 5, x, y, NULL,
                               NULL);
        CHECK(c, o != NULL);
        SHPPolygonComponents *psRes = NULL;
        CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
        CHECK(c, psRes->nComponents == 1);
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(o);
    }

    /* Unsupported shape type. */
    {
        const double x[] = {1, 2};
        const double y[] = {1, 2};
        SHPObject *o = pc_make(SHPT_POINT, -1, 0, NULL, 2, x, y, NULL, NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* Bad part ranges, rejected before coordinate access. */
    {
        const int anBadStart[] = {3, 5};
        const double x[] = {0, 4, 4, 0, 0, 0};
        const double y[] = {0, 0, 4, 4, 0, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anBadStart, 6, x, y,
                               NULL, NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }
    {
        const int anBadStart[] = {0, 0};
        const double x[] = {0, 4, 4, 0, 0, 0};
        const double y[] = {0, 0, 4, 4, 0, 0};
        SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, anBadStart, 6, x, y,
                               NULL, NULL);
        CHECK(c, o != NULL);
        pc_expect_failure(c, o);
        SHPDestroyObject(o);
    }

    /* NULL/argument handling. */
    {
        SHPPolygonComponents *psRes = NULL;
        char *pszError = NULL;
        CHECK(c, SHPBuildPolygonComponents(NULL, &psRes, &pszError) == 0);
        CHECK(c, psRes == NULL);
        CHECK(c, pszError != NULL);
        free(pszError);
        CHECK(c, SHPBuildPolygonComponents(NULL, NULL, NULL) == 0);
        SHPDestroyPolygonComponents(NULL); /* must be a no-op */
    }
}

/************************************************************************/
/*                         provenance group                             */
/************************************************************************/

/* Z/M follow their source vertices through reversal, including NoData M */
/* and a closing vertex whose Z/M differ from the first vertex.          */
static void test_zm_reversal(unit_context *c)
{
    const int anParts[] = {0};
    /* CCW square: the outer ring has to be reversed to clockwise. */
    const double x[] = {0, 20, 20, 0, 0};
    const double y[] = {0, 0, 20, 20, 0};
    const double z[] = {1000, 1001, 1002, 1003, 9999};
    const double m[] = {50, 51, 52, 53, -1e38};
    SHPObject *o = pc_make(SHPT_POLYGONZ, 7, 1, anParts, 5, x, y, z, m);
    CHECK(c, o != NULL);
    CHECK(c, o->bMeasureIsUsed);

    SHPPolygonComponents *psRes = NULL;
    CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
    CHECK(c, psRes->nComponents == 1);
    const SHPPolygonComponent *psC = &psRes->pasComponents[0];
    CHECK(c, psC->nRings == 1);
    const SHPPolygonRingInfo *psR = &psC->pasRings[0];
    CHECK(c, psR->bReversed == 1);
    CHECK(c, psR->nSrcPart == 0);
    CHECK(c, psR->nVertices == 5);

    /* [v0, v1, v2, v3, v4] reverses to [v0, v3, v2, v1, v4]: the first    */
    /* vertex stays put and the closing vertex keeps its own identity.     */
    const int anExpectedSrc[] = {0, 3, 2, 1, 4};
    for (int k = 0; k < 5; k++)
        CHECK(c, psR->panSrcVertex[k] == anExpectedSrc[k]);

    const SHPObject *psOut = psC->psObject;
    for (int k = 0; k < 5; k++)
    {
        const int nSrc = anExpectedSrc[k];
        CHECK(c, psOut->padfX[k] == x[nSrc]);
        CHECK(c, psOut->padfY[k] == y[nSrc]);
        CHECK(c, psOut->padfZ[k] == z[nSrc]);
        CHECK(c, psOut->padfM[k] == m[nSrc]);
    }
    CHECK(c, psOut->padfZ[4] == 9999);
    CHECK(c, psOut->padfM[4] == -1e38); /* NoData M copied as-is */
    CHECK(c, pc_ring_area(psOut, 0) < 0.0);

    /* Bounds match the actual output data. */
    double xmin = psOut->padfX[0], xmax = xmin, ymin = psOut->padfY[0],
           ymax = ymin, zmin = psOut->padfZ[0], zmax = zmin,
           mmin = psOut->padfM[0], mmax = mmin;
    for (int k = 1; k < 5; k++)
    {
        xmin = fmin(xmin, psOut->padfX[k]);
        xmax = fmax(xmax, psOut->padfX[k]);
        ymin = fmin(ymin, psOut->padfY[k]);
        ymax = fmax(ymax, psOut->padfY[k]);
        zmin = fmin(zmin, psOut->padfZ[k]);
        zmax = fmax(zmax, psOut->padfZ[k]);
        mmin = fmin(mmin, psOut->padfM[k]);
        mmax = fmax(mmax, psOut->padfM[k]);
    }
    CHECK(c, psOut->dfXMin == xmin && psOut->dfXMax == xmax);
    CHECK(c, psOut->dfYMin == ymin && psOut->dfYMax == ymax);
    CHECK(c, psOut->dfZMin == zmin && psOut->dfZMax == zmax);
    CHECK(c, psOut->dfMMin == mmin && psOut->dfMMax == mmax);
    CHECK(c, psOut->nShapeId == 7);

    SHPDestroyPolygonComponents(psRes);
    SHPDestroyObject(o);
}

/* Hole winding: a clockwise hole must be reversed to counter-clockwise. */
static void test_winding_and_order(unit_context *c)
{
    /* Outer CCW, hole clockwise (reversed square listing). */
    const int anParts[] = {0, 5};
    const double x[] = {0, 20, 20, 0, 0, 4, 4, 16, 16, 4};
    const double y[] = {0, 0, 20, 20, 0, 4, 16, 16, 4, 4};
    SHPObject *o = pc_make(SHPT_POLYGON, 3, 2, anParts, 10, x, y, NULL, NULL);
    CHECK(c, o != NULL);

    SHPPolygonComponents *psRes = NULL;
    CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
    CHECK(c, psRes->nComponents == 1);
    const SHPPolygonComponent *psC = &psRes->pasComponents[0];
    CHECK(c, psC->nRings == 2);
    CHECK(c, psC->pasRings[0].bReversed == 1); /* outer CCW -> CW */
    CHECK(c, psC->pasRings[1].bReversed == 1); /* hole CW -> CCW */
    CHECK(c, pc_ring_area(psC->psObject, 0) < 0.0);
    CHECK(c, pc_ring_area(psC->psObject, 1) > 0.0);
    /* First vertex of every ring is preserved. */
    CHECK(c, psC->psObject->padfX[0] == x[0]);
    CHECK(c, psC->psObject->padfY[0] == y[0]);
    const int nHoleStart = psC->psObject->panPartStart[1];
    CHECK(c, psC->psObject->padfX[nHoleStart] == x[5]);
    CHECK(c, psC->psObject->padfY[nHoleStart] == y[5]);
    SHPDestroyPolygonComponents(psRes);
    SHPDestroyObject(o);
}

static void test_source_immutable(unit_context *c)
{
    const int anParts[] = {0, 5};
    const double x[] = {0, 20, 20, 0, 0, 4, 16, 16, 4, 4};
    const double y[] = {0, 0, 20, 20, 0, 4, 4, 16, 16, 4};
    const size_t n = sizeof(x) / sizeof(x[0]);
    double *sx = malloc(n * sizeof(double));
    double *sy = malloc(n * sizeof(double));
    int *sp = malloc(2 * sizeof(int));
    memcpy(sx, x, n * sizeof(double));
    memcpy(sy, y, n * sizeof(double));
    memcpy(sp, anParts, 2 * sizeof(int));
    SHPObject *o = pc_make(SHPT_POLYGON, -1, 2, sp, (int)n, sx, sy, NULL,
                           NULL);
    free(sx);
    free(sy);
    free(sp);
    CHECK(c, o != NULL);

    SHPPolygonComponents *psRes = NULL;
    CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
    CHECK(c, o->nParts == 2 && o->nVertices == 10);
    for (size_t i = 0; i < n; i++)
    {
        CHECK(c, o->padfX[i] == x[i]);
        CHECK(c, o->padfY[i] == y[i]);
    }
    CHECK(c, o->panPartStart[0] == 0 && o->panPartStart[1] == 5);
    SHPDestroyPolygonComponents(psRes);
    SHPDestroyObject(o);
}

/* Same XY geometry in two objects with different Z/M: provenance must     */
/* resolve through source indices, never coordinate re-lookup.            */
static void test_same_xy_different_zm(unit_context *c)
{
    const int anParts[] = {0};
    const double x[] = {0, 4, 4, 0, 0};
    const double y[] = {0, 0, 4, 4, 0};
    const double zA[] = {1, 2, 3, 4, 1};
    const double zB[] = {10, 20, 30, 40, 10};
    const double mA[] = {100, 101, 102, 103, 100};
    const double mB[] = {200, 201, 202, 203, 200};

    SHPObject *oA = pc_make(SHPT_POLYGONZ, 0, 1, anParts, 5, x, y, zA, mA);
    SHPObject *oB = pc_make(SHPT_POLYGONZ, 1, 1, anParts, 5, x, y, zB, mB);
    CHECK(c, oA != NULL && oB != NULL);

    SHPPolygonComponents *rA = NULL, *rB = NULL;
    CHECK(c, SHPBuildPolygonComponents(oA, &rA, NULL) == 1);
    CHECK(c, SHPBuildPolygonComponents(oB, &rB, NULL) == 1);
    for (int k = 0; k < 5; k++)
    {
        const int nA = rA->pasComponents[0].pasRings[0].panSrcVertex[k];
        const int nB = rB->pasComponents[0].pasRings[0].panSrcVertex[k];
        CHECK(c, rA->pasComponents[0].psObject->padfZ[k] == zA[nA]);
        CHECK(c, rB->pasComponents[0].psObject->padfZ[k] == zB[nB]);
        CHECK(c, rA->pasComponents[0].psObject->padfM[k] == mA[nA]);
        CHECK(c, rB->pasComponents[0].psObject->padfM[k] == mB[nB]);
    }
    SHPDestroyPolygonComponents(rA);
    SHPDestroyPolygonComponents(rB);
    SHPDestroyObject(oA);
    SHPDestroyObject(oB);
}

/* Components own their data: destroy the source, write and read back. */
static void test_independent_ownership(unit_context *c)
{
    const char *pszTmp = getenv("TMPDIR");
    if (!pszTmp)
        pszTmp = "/tmp";
    char szPath[1024];
    snprintf(szPath, sizeof(szPath), "%s/shapelib_pc_own_%d", pszTmp,
             (int)getpid());

    const int anParts[] = {0};
    const double x[] = {1, 5, 5, 1, 1};
    const double y[] = {1, 1, 5, 5, 1};
    const double z[] = {7, 8, 9, 10, 7};
    const double m[] = {-1e38, 2, 3, 4, -1e38};
    SHPObject *o = pc_make(SHPT_POLYGONZ, 2, 1, anParts, 5, x, y, z, m);
    CHECK(c, o != NULL);

    SHPPolygonComponents *psRes = NULL;
    CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);
    SHPDestroyObject(o); /* source is gone; components must survive */

    SHPObject *psComp = psRes->pasComponents[0].psObject;
    SHPHandle hOut = SHPCreate(szPath, SHPT_POLYGONZ);
    CHECK(c, hOut != NULL);
    CHECK(c, SHPWriteObject(hOut, -1, psComp) >= 0);
    SHPClose(hOut);
    SHPDestroyPolygonComponents(psRes);

    SHPHandle hIn = SHPOpen(szPath, "rb");
    CHECK(c, hIn != NULL);
    int nEntities = 0, nType = 0;
    SHPGetInfo(hIn, &nEntities, &nType, NULL, NULL);
    CHECK(c, nEntities == 1);
    CHECK(c, nType == SHPT_POLYGONZ);
    SHPObject *psBack = SHPReadObject(hIn, 0);
    CHECK(c, psBack != NULL);
    CHECK(c, psBack->nVertices == 5);
    CHECK(c, psBack->padfZ[2] == 9);
    CHECK(c, psBack->padfM[0] == -1e38);
    SHPDestroyObject(psBack);
    SHPClose(hIn);
    {
        char szShp[1100], szShx[1100], szDbf[1100];
        snprintf(szShp, sizeof(szShp), "%s.shp", szPath);
        snprintf(szShx, sizeof(szShx), "%s.shx", szPath);
        snprintf(szDbf, sizeof(szDbf), "%s.dbf", szPath);
        remove(szShp);
        remove(szShx);
        remove(szDbf);
    }
}

static void test_empty_polygon(unit_context *c)
{
    SHPObject *o = SHPCreateObject(SHPT_POLYGONZ, -1, 0, NULL, NULL, 0, NULL,
                                   NULL, NULL, NULL);
    CHECK(c, o != NULL);
    SHPPolygonComponents *psRes = (SHPPolygonComponents *)(intptr_t)0x1;
    char *pszError = NULL;
    CHECK(c, SHPBuildPolygonComponents(o, &psRes, &pszError) == 1);
    CHECK(c, psRes != NULL);
    CHECK(c, psRes->nRings == 0);
    CHECK(c, psRes->nComponents == 0);
    CHECK(c, psRes->panParent == NULL);
    CHECK(c, psRes->panDepth == NULL);
    CHECK(c, psRes->pasComponents == NULL);
    CHECK(c, pszError == NULL);
    SHPDestroyPolygonComponents(psRes);
    SHPDestroyObject(o);
}

/************************************************************************/
/*                              files group                             */
/************************************************************************/

static void test_file_roundtrip(unit_context *c)
{
    const char *pszTmp = getenv("TMPDIR");
    if (!pszTmp)
        pszTmp = "/tmp";
    char szBase[1024], szOut[1024];
    snprintf(szBase, sizeof(szBase), "%s/shapelib_pc_base_%d", pszTmp,
             (int)getpid());
    snprintf(szOut, sizeof(szOut), "%s/shapelib_pc_out_%d", pszTmp,
             (int)getpid());

    /* --- Build a small SHPZ/DBF from scratch. --- */
    SHPHandle hShp = SHPCreate(szBase, SHPT_POLYGONZ);
    CHECK(c, hShp != NULL);
    DBFHandle hDbf = DBFCreate(szBase);
    CHECK(c, hDbf != NULL);
    CHECK(c, DBFAddField(hDbf, "srcid", FTString, 10, 0) == 0);

    /* Shape 0: one simple outer ring. */
    {
        const int anParts[] = {0};
        const double x[] = {0, 10, 10, 0, 0};
        const double y[] = {0, 0, 10, 10, 0};
        const double z[] = {1, 2, 3, 4, 1};
        const double m[] = {0, 0, 0, -1e38, 0};
        SHPObject *o = pc_make(SHPT_POLYGONZ, 0, 1, anParts, 5, x, y, z, m);
        CHECK(c, o != NULL);
        CHECK(c, SHPWriteObject(hShp, -1, o) >= 0);
        SHPDestroyObject(o);
        CHECK(c, DBFWriteStringAttribute(hDbf, 0, 0, "row-0"));
    }

    /* Shape 1: outer + hole + island -> two components, one source row. */
    {
        const int anParts[] = {0, 5, 10};
        const double x[] = {100, 120, 120, 100, 100, /* outer */
                            104, 116, 116, 104, 104, /* hole  */
                            108, 112, 112, 108, 108};/* island */
        const double y[] = {0, 0, 20, 20, 0,
                            4, 4, 16, 16, 4,
                            8, 8, 12, 12, 8};
        double z[15], m[15];
        for (int i = 0; i < 15; i++)
        {
            z[i] = 100.0 + i;
            m[i] = (i == 7) ? -1e38 : (double)i;
        }
        SHPObject *o = pc_make(SHPT_POLYGONZ, 1, 3, anParts, 15, x, y, z, m);
        CHECK(c, o != NULL);
        CHECK(c, SHPWriteObject(hShp, -1, o) >= 0);
        SHPDestroyObject(o);
        CHECK(c, DBFWriteStringAttribute(hDbf, 1, 0, "row-1"));
    }
    SHPClose(hShp);
    DBFClose(hDbf);

    /* --- Read back through the real IO path and split. --- */
    hShp = SHPOpen(szBase, "rb");
    CHECK(c, hShp != NULL);
    hDbf = DBFOpen(szBase, "rb");
    CHECK(c, hDbf != NULL);
    int nEntities = 0, nType = 0;
    SHPGetInfo(hShp, &nEntities, &nType, NULL, NULL);
    CHECK(c, nEntities == 2 && nType == SHPT_POLYGONZ);
    CHECK(c, DBFGetRecordCount(hDbf) == 2);

    const int nFieldCount = DBFGetFieldCount(hDbf);
    int nRecordLength = 1; /* deletion flag byte */
    for (int f = 0; f < nFieldCount; f++)
    {
        int nWidth = 0, nDecimals = 0;
        CHECK(c, DBFGetFieldInfo(hDbf, f, NULL, &nWidth, &nDecimals) !=
                FTInvalid);
        nRecordLength += nWidth;
    }

    SHPHandle hOutShp = SHPCreate(szOut, SHPT_POLYGONZ);
    CHECK(c, hOutShp != NULL);
    DBFHandle hOutDbf = DBFCloneEmpty(hDbf, szOut);
    CHECK(c, hOutDbf != NULL);

    int nWritten = 0;
    for (int iShape = 0; iShape < nEntities; iShape++)
    {
        SHPObject *o = SHPReadObject(hShp, iShape);
        CHECK(c, o != NULL);
        CHECK(c, o->nShapeId == iShape);
        SHPPolygonComponents *psRes = NULL;
        CHECK(c, SHPBuildPolygonComponents(o, &psRes, NULL) == 1);

        for (int iComp = 0; iComp < psRes->nComponents; iComp++)
        {
            SHPObject *psComp = psRes->pasComponents[iComp].psObject;
            /* In-memory components keep the source shape id, even when   */
            /* one source row splits into several output rows.             */
            CHECK(c, psComp->nShapeId == iShape);
            CHECK(c, SHPWriteObject(hOutShp, -1, psComp) >= 0);

            /* Copy the physical attribute row of the source nShapeId. */
            const char *pachTuple = DBFReadTuple(hDbf, psComp->nShapeId);
            CHECK(c, pachTuple != NULL);
            char *pachCopy = malloc((size_t)nRecordLength);
            CHECK(c, pachCopy != NULL);
            memcpy(pachCopy, pachTuple, (size_t)nRecordLength);
            CHECK(c, DBFWriteTuple(hOutDbf, nWritten, pachCopy));
            free(pachCopy);
            nWritten++;
        }
        SHPDestroyPolygonComponents(psRes);
        SHPDestroyObject(o);
    }
    SHPClose(hShp);
    DBFClose(hDbf);
    SHPClose(hOutShp);
    DBFClose(hOutDbf);

    /* 1 component from shape 0, 2 from shape 1. */
    CHECK(c, nWritten == 3);

    /* --- Reopen the split file and verify geometry and attributes. --- */
    hOutShp = SHPOpen(szOut, "rb");
    CHECK(c, hOutShp != NULL);
    hOutDbf = DBFOpen(szOut, "rb");
    CHECK(c, hOutDbf != NULL);
    SHPGetInfo(hOutShp, &nEntities, &nType, NULL, NULL);
    CHECK(c, nEntities == 3 && nType == SHPT_POLYGONZ);
    CHECK(c, DBFGetRecordCount(hOutDbf) == 3);

    SHPObject *o0 = SHPReadObject(hOutShp, 0);
    SHPObject *o1 = SHPReadObject(hOutShp, 1);
    SHPObject *o2 = SHPReadObject(hOutShp, 2);
    CHECK(c, o0 && o1 && o2);
    CHECK(c, o0->nParts == 1);
    CHECK(c, o1->nParts == 2); /* outer + hole */
    CHECK(c, o2->nParts == 1); /* island */
    CHECK(c, pc_ring_area(o1, 0) < 0.0);
    CHECK(c, pc_ring_area(o1, 1) > 0.0);
    /* NoData M survives the write/read round trip exactly. Shape 0's     */
    /* outer ring is reversed on output, so source vertex 3 (the NoData   */
    /* vertex) lands at output index 1.                                    */
    CHECK(c, o0->padfM[1] == -1e38);
    CHECK(c, o1->padfM[7] == -1e38);
    CHECK(c, o1->padfZ[0] >= 100.0 && o1->padfZ[0] <= 114.0);

    /* The split rows both trace back to physical source row 1. */
    CHECK(c, strcmp(DBFReadStringAttribute(hOutDbf, 0, 0), "row-0") == 0);
    CHECK(c, strcmp(DBFReadStringAttribute(hOutDbf, 1, 0), "row-1") == 0);
    CHECK(c, strcmp(DBFReadStringAttribute(hOutDbf, 2, 0), "row-1") == 0);

    SHPDestroyObject(o0);
    SHPDestroyObject(o1);
    SHPDestroyObject(o2);
    SHPClose(hOutShp);
    DBFClose(hOutDbf);

    {
        char szExt[1100];
        const char *const apszExt[] = {".shp", ".shx", ".dbf"};
        for (size_t i = 0; i < sizeof(apszExt) / sizeof(apszExt[0]); i++)
        {
            snprintf(szExt, sizeof(szExt), "%s%s", szBase, apszExt[i]);
            remove(szExt);
            snprintf(szExt, sizeof(szExt), "%s%s", szOut, apszExt[i]);
            remove(szExt);
        }
    }
}

/************************************************************************/
/*                           Registration                               */
/************************************************************************/

const unit_case unit_cases[] = {
    {"hierarchy", "unordered_rings", test_unordered_rings},
    {"hierarchy", "disjoint_parents", test_disjoint_parents},
    {"hierarchy", "concave_bbox_trap", test_concave_bbox_trap},

    {"predicates", "ring_validity", test_ring_validity},
    {"predicates", "ring_contacts", test_ring_contacts},
    {"predicates", "ranges_and_types", test_ranges_and_types},

    {"provenance", "zm_reversal", test_zm_reversal},
    {"provenance", "winding_and_order", test_winding_and_order},
    {"provenance", "source_immutable", test_source_immutable},
    {"provenance", "same_xy_different_zm", test_same_xy_different_zm},
    {"provenance", "independent_ownership", test_independent_ownership},
    {"provenance", "empty_polygon", test_empty_polygon},

    {"files", "roundtrip_split_attributes", test_file_roundtrip},
};

const unsigned unit_case_count =
    sizeof(unit_cases) / sizeof(unit_cases[0]);
