/******************************************************************************
 *
 * Project:  Shapelib
 * Purpose:  Decompose a polygon SHPObject, whose rings may be stored in any
 *           order and with any winding direction, into independent polygon
 *           components (one outer ring and its direct holes each), while
 *           keeping the provenance of every output ring and vertex.
 * Author:   Shapelib contributors
 *
 ******************************************************************************
 * Copyright (c) 2026, Shapelib contributors
 *
 * SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
 ******************************************************************************
 *
 */

#include "shapefil_private.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif

#define PC_COORD_MIN 9.31322574615478515625e-10  /* 2^-30 */
#define PC_COORD_MAX 1073741824.0                /* 2^30 */

/************************************************************************/
/*                            PC geometry types                         */
/************************************************************************/

typedef struct
{
    int nPart;  /* source part index */
    int nStart; /* first source vertex index */
    int nCount; /* vertex count, including the closing vertex */
    double dfArea2; /* signed doubled area, in source orientation */
} PCRing;

/************************************************************************/
/*                          Small helper routines                       */
/************************************************************************/

static char *PCStrdup(const char *pszText)
{
    const size_t nSize = strlen(pszText) + 1;
    char *pszResult = STATIC_CAST(char *, malloc(nSize));
    if (pszResult)
        memcpy(pszResult, pszText, nSize);
    return pszResult;
}

static int PCSetError(char **ppszError, char **ppszMessage, const char *fmt, ...)
{
    char szBuffer[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(szBuffer, sizeof(szBuffer), fmt, args);
    va_end(args);
    szBuffer[sizeof(szBuffer) - 1] = '\0';
    free(*ppszMessage);
    *ppszMessage = PCStrdup(szBuffer);
    if (ppszError)
    {
        free(*ppszError);
        *ppszError = PCStrdup(szBuffer);
    }
    return FALSE;
}

static double PCMin(double a, double b)
{
    return a < b ? a : b;
}

static double PCMax(double a, double b)
{
    return a > b ? a : b;
}

/* Cross product of vectors AB and AC. Exact IEEE double evaluation: no   */
/* epsilon is applied anywhere in this file.                               */
static double PCCross(double ax, double ay, double bx, double by, double cx,
                      double cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static int PCPointOnSegment(double px, double py, double ax, double ay,
                            double bx, double by)
{
    if (PCCross(ax, ay, bx, by, px, py) != 0.0)
        return FALSE;
    return PCMin(ax, bx) <= px && px <= PCMax(ax, bx) &&
           PCMin(ay, by) <= py && py <= PCMax(ay, by);
}

/* Segment to segment relationship.                                       */
/* Returns 0 when disjoint, 1 for a proper interior crossing and 2 for    */
/* any non-proper contact (endpoint lying on the other segment or a       */
/* collinear overlap).                                                     */
static int PCSegmentRelate(double ax, double ay, double bx, double by,
                           double px, double py, double qx, double qy)
{
    const double c1 = PCCross(ax, ay, bx, by, px, py);
    const double c2 = PCCross(ax, ay, bx, by, qx, qy);
    const double c3 = PCCross(px, py, qx, qy, ax, ay);
    const double c4 = PCCross(px, py, qx, qy, bx, by);

    if (((c1 > 0.0 && c2 < 0.0) || (c1 < 0.0 && c2 > 0.0)) &&
        ((c3 > 0.0 && c4 < 0.0) || (c3 < 0.0 && c4 > 0.0)))
        return 1;

    if (c1 == 0.0 && PCPointOnSegment(px, py, ax, ay, bx, by))
        return 2;
    if (c2 == 0.0 && PCPointOnSegment(qx, qy, ax, ay, bx, by))
        return 2;
    if (c3 == 0.0 && PCPointOnSegment(ax, ay, px, py, qx, qy))
        return 2;
    if (c4 == 0.0 && PCPointOnSegment(bx, by, px, py, qx, qy))
        return 2;

    return 0;
}

static double PCRingArea2(const SHPObject *psObject, int nStart, int nCount)
{
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

/* Strict point in ring test: returns 1 for an interior point, 0 for an   */
/* exterior point or a point on the ring boundary.                         */
static int PCPointInRingStrict(const SHPObject *psObject, int nStart,
                               int nCount, double dfTestX, double dfTestY)
{
    int bInside = FALSE;
    for (int i = 0; i < nCount - 1; i++)
    {
        const double y0 = psObject->padfY[nStart + i];
        const double y1 = psObject->padfY[nStart + i + 1];
        if ((y0 > dfTestY) != (y1 > dfTestY))
        {
            const double x0 = psObject->padfX[nStart + i];
            const double x1 = psObject->padfX[nStart + i + 1];
            const double dfXInter =
                x0 + (dfTestY - y0) / (y1 - y0) * (x1 - x0);
            if (dfXInter == dfTestX)
                return FALSE; /* on the boundary */
            if (dfXInter > dfTestX)
                bInside = !bInside;
        }
    }
    return bInside;
}

/* Returns TRUE when ring nCandidatePart strictly contains ring nPart.    */
/* The rings are known to be pairwise disjoint before this is called.     */
static int PCRingContainsRing(const SHPObject *psObject,
                              const PCRing *psCandidate,
                              const PCRing *psRing)
{
    /* Use the midpoint of the first edge so a shared vertex situation    */
    /* cannot confuse the test, and then confirm every vertex is inside.  */
    const double dfMidX =
        (psObject->padfX[psRing->nStart] +
         psObject->padfX[psRing->nStart + 1]) *
        0.5;
    const double dfMidY =
        (psObject->padfY[psRing->nStart] +
         psObject->padfY[psRing->nStart + 1]) *
        0.5;
    if (!PCPointInRingStrict(psObject, psCandidate->nStart,
                             psCandidate->nCount, dfMidX, dfMidY))
        return FALSE;

    for (int i = 0; i < psRing->nCount - 1; i++)
    {
        if (!PCPointInRingStrict(psObject, psCandidate->nStart,
                                 psCandidate->nCount,
                                 psObject->padfX[psRing->nStart + i],
                                 psObject->padfY[psRing->nStart + i]))
            return FALSE;
    }
    return TRUE;
}

/************************************************************************/
/*                       Per-ring validity checks                       */
/************************************************************************/

static int PCValidateRing(const SHPObject *psObject, PCRing *psRing,
                          char **ppszError, char **ppszMessage)
{
    const int nStart = psRing->nStart;
    const int nCount = psRing->nCount;

    /* Explicit closure on the exact represented doubles. */
    if (psObject->padfX[nStart] !=
            psObject->padfX[nStart + nCount - 1] ||
        psObject->padfY[nStart] !=
            psObject->padfY[nStart + nCount - 1])
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: ring of source part %d "
                          "is not explicitly closed (first vertex %d)",
                          psRing->nPart, nStart);
    }

    /* At least three distinct XY points (the closing vertex duplicates   */
    /* the first one).                                                      */
    int nDistinct = 0;
    for (int i = 0; i < nCount - 1; i++)
    {
        int bSeen = FALSE;
        for (int j = 0; j < i; j++)
        {
            if (psObject->padfX[nStart + i] ==
                    psObject->padfX[nStart + j] &&
                psObject->padfY[nStart + i] ==
                    psObject->padfY[nStart + j])
            {
                bSeen = TRUE;
                break;
            }
        }
        if (!bSeen)
            nDistinct++;
    }
    if (nDistinct < 3)
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: ring of source part %d "
                          "has fewer than 3 distinct XY points (vertex %d)",
                          psRing->nPart, nStart);
    }

    /* No zero-length edge. */
    for (int i = 0; i < nCount - 1; i++)
    {
        if (psObject->padfX[nStart + i] ==
                psObject->padfX[nStart + i + 1] &&
            psObject->padfY[nStart + i] ==
                psObject->padfY[nStart + i + 1])
        {
            return PCSetError(ppszError, ppszMessage,
                              "SHPBuildPolygonComponents: zero-length edge in "
                              "source part %d at edge start vertex %d",
                              psRing->nPart, nStart + i);
        }
    }

    psRing->dfArea2 = PCRingArea2(psObject, nStart, nCount);
    if (psRing->dfArea2 == 0.0)
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: ring of source part %d "
                          "has zero area (edge start vertex %d)",
                          psRing->nPart, nStart);
    }

    /* Adjacent edges: a collinear fold (spike or backtrack) is illegal. A */
    /* straight continuation through three collinear points is allowed.    */
    for (int i = 0; i < nCount - 1; i++)
    {
        const int j = (i + 1) % (nCount - 1);
        const double ax = psObject->padfX[nStart + i];
        const double ay = psObject->padfY[nStart + i];
        const double bx = psObject->padfX[nStart + i + 1];
        const double by = psObject->padfY[nStart + i + 1];
        const double cx = psObject->padfX[nStart + j + 1];
        const double cy = psObject->padfY[nStart + j + 1];
        if (PCCross(ax, ay, bx, by, cx, cy) == 0.0)
        {
            const double dfDot =
                (bx - ax) * (cx - bx) + (by - ay) * (cy - by);
            if (dfDot <= 0.0)
            {
                return PCSetError(
                    ppszError, ppszMessage,
                    "SHPBuildPolygonComponents: collinear fold in source "
                    "part %d at edge start vertex %d",
                    psRing->nPart, nStart + i);
            }
        }
    }

    /* Non-adjacent edges must not cross or touch. */
    const int nEdges = nCount - 1;
    for (int i = 0; i < nEdges; i++)
    {
        for (int j = i + 2; j < nEdges; j++)
        {
            /* Edges 0 and nEdges-1 share the first/last vertex and are    */
            /* adjacent, so skip that single pair as well.                 */
            if (i == 0 && j == nEdges - 1)
                continue;
            const int nRel = PCSegmentRelate(
                psObject->padfX[nStart + i],
                psObject->padfY[nStart + i],
                psObject->padfX[nStart + i + 1],
                psObject->padfY[nStart + i + 1],
                psObject->padfX[nStart + j],
                psObject->padfY[nStart + j],
                psObject->padfX[nStart + j + 1],
                psObject->padfY[nStart + j + 1]);
            if (nRel != 0)
            {
                return PCSetError(
                    ppszError, ppszMessage,
                    "SHPBuildPolygonComponents: self-intersection in source "
                    "part %d between edges starting at vertices %d and %d",
                    psRing->nPart, nStart + i, nStart + j);
            }
        }
    }

    return TRUE;
}

/************************************************************************/
/*                    Pairwise ring contact check                       */
/************************************************************************/

static int PCValidateRingPairs(const SHPObject *psObject,
                               const PCRing *pasRings, int nRingCount,
                               char **ppszError, char **ppszMessage)
{
    double *padfBox =
        STATIC_CAST(double *,
                    malloc(sizeof(double) * 4 *
                           STATIC_CAST(size_t, nRingCount)));
    if (!padfBox)
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: memory allocation "
                          "failure");
    }

    for (int r = 0; r < nRingCount; r++)
    {
        const PCRing *psR = &pasRings[r];
        double dfMinX = psObject->padfX[psR->nStart];
        double dfMaxX = dfMinX;
        double dfMinY = psObject->padfY[psR->nStart];
        double dfMaxY = dfMinY;
        for (int k = 1; k < psR->nCount; k++)
        {
            const double x = psObject->padfX[psR->nStart + k];
            const double y = psObject->padfY[psR->nStart + k];
            dfMinX = PCMin(dfMinX, x);
            dfMaxX = PCMax(dfMaxX, x);
            dfMinY = PCMin(dfMinY, y);
            dfMaxY = PCMax(dfMaxY, y);
        }
        padfBox[4 * r + 0] = dfMinX;
        padfBox[4 * r + 1] = dfMinY;
        padfBox[4 * r + 2] = dfMaxX;
        padfBox[4 * r + 3] = dfMaxY;
    }

    int bOK = TRUE;
    for (int i = 0; i < nRingCount && bOK; i++)
    {
        const PCRing *psA = &pasRings[i];
        for (int j = i + 1; j < nRingCount && bOK; j++)
        {
            const PCRing *psB = &pasRings[j];

            /* Bounding-box rejection on exact values: disjoint boxes mean  */
            /* the rings can neither intersect nor contain one another.    */
            if (padfBox[4 * i + 2] < padfBox[4 * j + 0] ||
                padfBox[4 * j + 2] < padfBox[4 * i + 0] ||
                padfBox[4 * i + 3] < padfBox[4 * j + 1] ||
                padfBox[4 * j + 3] < padfBox[4 * i + 1])
                continue;

            for (int ei = 0; ei < psA->nCount - 1; ei++)
            {
                for (int ej = 0; ej < psB->nCount - 1; ej++)
                {
                    const int nRel = PCSegmentRelate(
                        psObject->padfX[psA->nStart + ei],
                        psObject->padfY[psA->nStart + ei],
                        psObject->padfX[psA->nStart + ei + 1],
                        psObject->padfY[psA->nStart + ei + 1],
                        psObject->padfX[psB->nStart + ej],
                        psObject->padfY[psB->nStart + ej],
                        psObject->padfX[psB->nStart + ej + 1],
                        psObject->padfY[psB->nStart + ej + 1]);
                    if (nRel != 0)
                    {
                        PCSetError(
                            ppszError, ppszMessage,
                            "SHPBuildPolygonComponents: source parts %d and "
                            "%d cross, overlap or touch (edge start vertices "
                            "%d and %d)",
                            psA->nPart, psB->nPart,
                            psA->nStart + ei, psB->nStart + ej);
                        bOK = FALSE;
                        break;
                    }
                }
                if (!bOK)
                    break;
            }
        }
    }

    free(padfBox);
    return bOK;
}

/************************************************************************/
/*                      Containment hierarchy build                     */
/************************************************************************/

static int PCBuildHierarchy(const SHPObject *psObject, PCRing *pasRings,
                            int nRingCount, int *panParent, int *panDepth,
                            char **ppszError, char **ppszMessage)
{
    int *panContains =
        STATIC_CAST(int *, calloc(STATIC_CAST(size_t, nRingCount) * nRingCount,
                                  sizeof(int)));
    if (!panContains)
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: memory allocation "
                          "failure");
    }

    for (int i = 0; i < nRingCount; i++)
        panParent[i] = -1;

    int bOK = TRUE;
    for (int i = 0; i < nRingCount && bOK; i++)
    {
        int nParent = -1;
        for (int j = 0; j < nRingCount; j++)
        {
            if (i == j)
                continue;
            if (PCRingContainsRing(psObject, &pasRings[j], &pasRings[i]))
            {
                panContains[i * nRingCount + j] = TRUE;
                /* The parent is the innermost containing ring: keep the   */
                /* candidate that is itself contained in every other one.  */
                if (nParent < 0)
                {
                    nParent = j;
                }
                else if (PCRingContainsRing(psObject, &pasRings[nParent],
                                            &pasRings[j]))
                {
                    nParent = j;
                }
            }
        }
        panParent[i] = nParent;
    }

    /* Resolve depths and verify that every containing ring is an ancestor */
    /* through parent links (consistent nesting forest, no cycles).        */
    if (bOK)
    {
        for (int i = 0; i < nRingCount; i++)
        {
            int nDepth = 0;
            int nCursor = panParent[i];
            int nSteps = 0;
            while (nCursor >= 0)
            {
                nDepth++;
                nCursor = panParent[nCursor];
                nSteps++;
                if (nSteps > nRingCount)
                {
                    PCSetError(ppszError, ppszMessage,
                               "SHPBuildPolygonComponents: ring containment "
                               "cycle involving source part %d",
                               pasRings[i].nPart);
                    bOK = FALSE;
                    break;
                }
            }
            if (!bOK)
                break;
            panDepth[i] = nDepth;
        }
    }

    if (bOK)
    {
        for (int i = 0; i < nRingCount && bOK; i++)
        {
            for (int j = 0; j < nRingCount; j++)
            {
                if (!panContains[i * nRingCount + j])
                    continue;
                /* Walk the parent chain of i; j must be on it. */
                int nCursor = panParent[i];
                int bAncestor = FALSE;
                while (nCursor >= 0)
                {
                    if (nCursor == j)
                    {
                        bAncestor = TRUE;
                        break;
                    }
                    nCursor = panParent[nCursor];
                }
                if (!bAncestor)
                {
                    PCSetError(ppszError, ppszMessage,
                               "SHPBuildPolygonComponents: source part %d is "
                               "contained in part %d but the rings do not "
                               "form a nested hierarchy",
                               pasRings[i].nPart, pasRings[j].nPart);
                    bOK = FALSE;
                    break;
                }
            }
        }
    }

    free(panContains);
    return bOK;
}

/************************************************************************/
/*                          Result construction                         */
/************************************************************************/

static void PCDestroyPartial(SHPPolygonComponents *psComponents)
{
    if (!psComponents)
        return;
    if (psComponents->pasComponents)
    {
        for (int i = 0; i < psComponents->nComponents; i++)
        {
            SHPPolygonComponent *psC = &psComponents->pasComponents[i];
            if (psC->pasRings)
            {
                for (int j = 0; j < psC->nRings; j++)
                    free(psC->pasRings[j].panSrcVertex);
                free(psC->pasRings);
            }
            if (psC->psObject)
                SHPDestroyObject(psC->psObject);
        }
        free(psComponents->pasComponents);
    }
    free(psComponents->panParent);
    free(psComponents->panDepth);
    free(psComponents);
}

/* Gather the rings (outer first, then holes by source part index) of the */
/* component whose outer ring is nOuterPart.                               */
static int PCCollectComponentRings(int nOuterPart, const int *panParent,
                                   const int *panDepth, int nRingCount,
                                   int *panOutParts, int *pnOutRingCount)
{
    int nRingCountOut = 0;
    panOutParts[nRingCountOut++] = nOuterPart;
    for (int i = 0; i < nRingCount; i++)
    {
        if (panParent[i] == nOuterPart &&
            panDepth[i] == panDepth[nOuterPart] + 1)
            panOutParts[nRingCountOut++] = i;
    }
    /* Sort the holes by source part index; keep the outer in position 0. */
    for (int i = 2; i < nRingCountOut; i++)
    {
        for (int j = i; j > 1 && panOutParts[j - 1] > panOutParts[j]; j--)
        {
            const int nTmp = panOutParts[j - 1];
            panOutParts[j - 1] = panOutParts[j];
            panOutParts[j] = nTmp;
        }
    }
    *pnOutRingCount = nRingCountOut;
    return TRUE;
}

static int PCBuildComponent(const SHPObject *psObject, const PCRing *pasRings,
                            const int *panParent, const int *panDepth,
                            int nOuterPart, SHPPolygonComponent *psComponent,
                            char **ppszError, char **ppszMessage)
{
    int *panOutParts =
        STATIC_CAST(int *, malloc(sizeof(int) * psObject->nParts));
    int nRingCountOut = 0;
    if (!panOutParts)
    {
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: memory allocation "
                          "failure");
    }
    PCCollectComponentRings(nOuterPart, panParent, panDepth,
                            psObject->nParts, panOutParts, &nRingCountOut);

    int nVerticesOut = 0;
    for (int i = 0; i < nRingCountOut; i++)
        nVerticesOut += pasRings[panOutParts[i]].nCount;

    double *padfX = STATIC_CAST(double *,
                                malloc(sizeof(double) * nVerticesOut));
    double *padfY = STATIC_CAST(double *,
                                malloc(sizeof(double) * nVerticesOut));
    double *padfZ = STATIC_CAST(double *,
                                malloc(sizeof(double) * nVerticesOut));
    double *padfM = STATIC_CAST(double *,
                                malloc(sizeof(double) * nVerticesOut));
    int *panPartStart =
        STATIC_CAST(int *, malloc(sizeof(int) * nRingCountOut));
    SHPPolygonRingInfo *pasRingInfo =
        STATIC_CAST(SHPPolygonRingInfo *,
                    calloc(STATIC_CAST(size_t, nRingCountOut),
                           sizeof(SHPPolygonRingInfo)));
    if (!padfX || !padfY || !padfZ || !padfM || !panPartStart || !pasRingInfo)
    {
        free(padfX);
        free(padfY);
        free(padfZ);
        free(padfM);
        free(panPartStart);
        free(pasRingInfo);
        return PCSetError(ppszError, ppszMessage,
                          "SHPBuildPolygonComponents: memory allocation "
                          "failure");
    }

    const int bHasZ = (psObject->nSHPType == SHPT_POLYGONZ);
    const int bUseM = psObject->bMeasureIsUsed;

    int nVertexCursor = 0;
    int bFailed = FALSE;
    for (int i = 0; i < nRingCountOut; i++)
    {
        const int nSrcPart = panOutParts[i];
        const PCRing *psRing = &pasRings[nSrcPart];
        const int nStart = psRing->nStart;
        const int nCount = psRing->nCount;

        panPartStart[i] = nVertexCursor;
        pasRingInfo[i].nSrcPart = nSrcPart;
        pasRingInfo[i].nVertices = nCount;
        pasRingInfo[i].panSrcVertex =
            STATIC_CAST(int *, malloc(sizeof(int) * nCount));
        if (!pasRingInfo[i].panSrcVertex)
        {
            PCSetError(ppszError, ppszMessage,
                       "SHPBuildPolygonComponents: memory allocation "
                       "failure");
            bFailed = TRUE;
            break;
        }

        /* Outer rings (even depth) come out clockwise (negative signed    */
        /* area), holes (odd depth) counter-clockwise.                      */
        const int bWantClockwise = (panDepth[nSrcPart] % 2) == 0;
        const int bReverse =
            bWantClockwise ? (psRing->dfArea2 > 0.0)
                           : (psRing->dfArea2 < 0.0);
        pasRingInfo[i].bReversed = bReverse;

        for (int k = 0; k < nCount; k++)
        {
            /* Keep the first vertex fixed and the ring explicitly closed: */
            /* [v0, v1, ..., v_{n-2}, v_{n-1}] reverses to                 */
            /* [v0, v_{n-2}, ..., v1, v_{n-1}], so the last output vertex  */
            /* corresponds to the original closing vertex v_{n-1} (which   */
            /* duplicates v0).                                             */
            int nSrcK;
            if (!bReverse || k == 0)
                nSrcK = k;
            else if (k == nCount - 1)
                nSrcK = nCount - 1;
            else
                nSrcK = nCount - 1 - k;
            const int nSrcVertex = nStart + nSrcK;
            const int nDstVertex = nVertexCursor + k;

            padfX[nDstVertex] = psObject->padfX[nSrcVertex];
            padfY[nDstVertex] = psObject->padfY[nSrcVertex];
            if (bHasZ)
                padfZ[nDstVertex] = psObject->padfZ[nSrcVertex];
            if (bUseM)
                padfM[nDstVertex] = psObject->padfM[nSrcVertex];
            pasRingInfo[i].panSrcVertex[k] = nSrcVertex;
        }
        nVertexCursor += nCount;
    }

    SHPObject *psOut = SHPLIB_NULLPTR;
    if (!bFailed)
    {
        psOut = SHPCreateObject(
            psObject->nSHPType, psObject->nShapeId, nRingCountOut,
            panPartStart, SHPLIB_NULLPTR, nVerticesOut, padfX, padfY,
            bHasZ ? padfZ : SHPLIB_NULLPTR,
            bUseM ? padfM : SHPLIB_NULLPTR);
        if (!psOut)
        {
            PCSetError(ppszError, ppszMessage,
                       "SHPBuildPolygonComponents: memory allocation "
                       "failure");
            bFailed = TRUE;
        }
    }

    free(padfX);
    free(padfY);
    free(padfZ);
    free(padfM);
    free(panPartStart);
    free(panOutParts);

    if (bFailed)
    {
        for (int i = 0; i < nRingCountOut; i++)
            free(pasRingInfo[i].panSrcVertex);
        free(pasRingInfo);
        return FALSE;
    }

    psComponent->psObject = psOut;
    psComponent->nOuterSrcPart = nOuterPart;
    psComponent->nRings = nRingCountOut;
    psComponent->pasRings = pasRingInfo;
    return TRUE;
}

/************************************************************************/
/*                      SHPBuildPolygonComponents()                     */
/************************************************************************/

int SHPAPI_CALL SHPBuildPolygonComponents(
    const SHPObject *psObject, SHPPolygonComponents **pppsComponents,
    char **ppszError)
{
    char *pszMessage = SHPLIB_NULLPTR;

    if (ppszError)
        *ppszError = SHPLIB_NULLPTR;
    if (!pppsComponents)
        return FALSE;
    *pppsComponents = SHPLIB_NULLPTR;

    if (!psObject)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: NULL input SHPObject");
        return FALSE;
    }

    if (psObject->nSHPType != SHPT_POLYGON &&
        psObject->nSHPType != SHPT_POLYGONZ &&
        psObject->nSHPType != SHPT_POLYGONM)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: unsupported shape type; only "
                "SHPT_POLYGON, SHPT_POLYGONZ and SHPT_POLYGONM are accepted");
        return FALSE;
    }

    /* ----------------------------------------------------------------- */
    /* Structural checks, before any coordinate is accessed.             */
    /* ----------------------------------------------------------------- */
    if (psObject->nParts < 0 || psObject->nVertices < 0)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: negative part or vertex count");
        return FALSE;
    }
    if (psObject->nParts == 0 || psObject->nVertices == 0)
    {
        /* Empty polygon: valid empty result. */
        SHPPolygonComponents *psEmpty =
            STATIC_CAST(SHPPolygonComponents *,
                        calloc(1, sizeof(SHPPolygonComponents)));
        if (!psEmpty)
        {
            if (ppszError)
                *ppszError = PCStrdup(
                    "SHPBuildPolygonComponents: memory allocation failure");
            return FALSE;
        }
        *pppsComponents = psEmpty;
        return TRUE;
    }
    if (!psObject->panPartStart || !psObject->padfX || !psObject->padfY)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: malformed SHPObject: missing "
                "part offsets or coordinate arrays");
        return FALSE;
    }
    if (psObject->panPartStart[0] != 0)
    {
        if (ppszError)
            *ppszError = PCStrdup(
            "SHPBuildPolygonComponents: first part does not start at "
            "vertex 0");
        return FALSE;
    }
    for (int i = 1; i < psObject->nParts; i++)
    {
        if (psObject->panPartStart[i] <= psObject->panPartStart[i - 1] ||
            psObject->panPartStart[i] > psObject->nVertices)
        {
            if (ppszError)
            {
                char szBuffer[200];
                snprintf(szBuffer, sizeof(szBuffer),
                         "SHPBuildPolygonComponents: bad part range at "
                         "source part %d (part start %d)",
                         i, psObject->panPartStart[i]);
                *ppszError = PCStrdup(szBuffer);
            }
            return FALSE;
        }
    }

    PCRing *pasRings =
        STATIC_CAST(PCRing *, calloc(STATIC_CAST(size_t, psObject->nParts),
                                     sizeof(PCRing)));
    if (!pasRings)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: memory allocation failure");
        return FALSE;
    }
    for (int i = 0; i < psObject->nParts; i++)
    {
        pasRings[i].nPart = i;
        pasRings[i].nStart = psObject->panPartStart[i];
        pasRings[i].nCount =
            (i == psObject->nParts - 1)
                ? (psObject->nVertices - psObject->panPartStart[i])
                : (psObject->panPartStart[i + 1] -
                   psObject->panPartStart[i]);
        if (pasRings[i].nCount <= 0 ||
            pasRings[i].nStart + pasRings[i].nCount > psObject->nVertices)
        {
            if (ppszError)
            {
                char szBuffer[200];
                snprintf(szBuffer, sizeof(szBuffer),
                         "SHPBuildPolygonComponents: bad part range at "
                         "source part %d",
                         i);
                *ppszError = PCStrdup(szBuffer);
            }
            free(pasRings);
            return FALSE;
        }
    }

    /* ----------------------------------------------------------------- */
    /* Coordinate finiteness and supported range.                        */
    /* ----------------------------------------------------------------- */
    for (int i = 0; i < psObject->nVertices; i++)
    {
        const double x = psObject->padfX[i];
        const double y = psObject->padfY[i];
        if (!isfinite(x) || !isfinite(y))
        {
            free(pasRings);
            if (ppszError)
            {
                char szBuffer[200];
                snprintf(szBuffer, sizeof(szBuffer),
                         "SHPBuildPolygonComponents: non-finite X/Y at "
                         "source vertex %d",
                         i);
                *ppszError = PCStrdup(szBuffer);
            }
            return FALSE;
        }
        if ((x != 0.0 &&
             (fabs(x) < PC_COORD_MIN || fabs(x) > PC_COORD_MAX)) ||
            (y != 0.0 &&
             (fabs(y) < PC_COORD_MIN || fabs(y) > PC_COORD_MAX)))
        {
            free(pasRings);
            if (ppszError)
            {
                char szBuffer[200];
                snprintf(szBuffer, sizeof(szBuffer),
                         "SHPBuildPolygonComponents: coordinate outside the "
                         "supported range [2^-30, 2^30] at source vertex %d",
                         i);
                *ppszError = PCStrdup(szBuffer);
            }
            return FALSE;
        }
    }

    /* ----------------------------------------------------------------- */
    /* Per-ring and pairwise ring validation.                            */
    /* ----------------------------------------------------------------- */
    for (int i = 0; i < psObject->nParts; i++)
    {
        if (!PCValidateRing(psObject, &pasRings[i], ppszError, &pszMessage))
        {
            free(pasRings);
            return FALSE;
        }
    }
    if (!PCValidateRingPairs(psObject, pasRings, psObject->nParts, ppszError,
                             &pszMessage))
    {
        free(pasRings);
        return FALSE;
    }

    /* ----------------------------------------------------------------- */
    /* Containment hierarchy.                                            */
    /* ----------------------------------------------------------------- */
    int *panParent =
        STATIC_CAST(int *, malloc(sizeof(int) * psObject->nParts));
    int *panDepth =
        STATIC_CAST(int *, malloc(sizeof(int) * psObject->nParts));
    if (!panParent || !panDepth)
    {
        free(panParent);
        free(panDepth);
        free(pasRings);
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: memory allocation failure");
        return FALSE;
    }
    if (!PCBuildHierarchy(psObject, pasRings, psObject->nParts, panParent,
                          panDepth, ppszError, &pszMessage))
    {
        free(panParent);
        free(panDepth);
        free(pasRings);
        return FALSE;
    }

    /* ----------------------------------------------------------------- */
    /* Allocate the result.                                              */
    /* ----------------------------------------------------------------- */
    int nComponentCount = 0;
    for (int i = 0; i < psObject->nParts; i++)
    {
        if ((panDepth[i] % 2) == 0)
            nComponentCount++;
    }

    SHPPolygonComponents *psComponents =
        STATIC_CAST(SHPPolygonComponents *,
                    calloc(1, sizeof(SHPPolygonComponents)));
    if (!psComponents)
    {
        free(panParent);
        free(panDepth);
        free(pasRings);
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: memory allocation failure");
        return FALSE;
    }
    psComponents->nRings = psObject->nParts;
    psComponents->panParent = panParent;
    psComponents->panDepth = panDepth;
    psComponents->nComponents = nComponentCount;
    psComponents->pasComponents =
        STATIC_CAST(SHPPolygonComponent *,
                    calloc(STATIC_CAST(size_t, nComponentCount),
                           sizeof(SHPPolygonComponent)));
    if (!psComponents->pasComponents)
    {
        if (ppszError)
            *ppszError = PCStrdup(
                "SHPBuildPolygonComponents: memory allocation failure");
        PCDestroyPartial(psComponents);
        free(pasRings);
        return FALSE;
    }

    /* Even-depth rings in source part order are the component outer      */
    /* rings, which also orders components by outer source part index.    */
    int nComponent = 0;
    for (int i = 0; i < psObject->nParts; i++)
    {
        if ((panDepth[i] % 2) != 0)
            continue;
        if (!PCBuildComponent(psObject, pasRings, panParent, panDepth, i,
                              &psComponents->pasComponents[nComponent],
                              ppszError, &pszMessage))
        {
            PCDestroyPartial(psComponents);
            free(pasRings);
            return FALSE;
        }
        nComponent++;
    }

    free(pasRings);
    free(pszMessage);
    *pppsComponents = psComponents;
    return TRUE;
}

/************************************************************************/
/*                     SHPDestroyPolygonComponents()                    */
/************************************************************************/

void SHPAPI_CALL
    SHPDestroyPolygonComponents(SHPPolygonComponents *psComponents)
{
    if (!psComponents)
        return;
    PCDestroyPartial(psComponents);
}
