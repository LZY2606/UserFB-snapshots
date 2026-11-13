/******************************************************************************
 *
 * Project:  Shapelib
 * Purpose:  Example C API use of SHPBuildPolygonComponents().
 *
 *           Splits every polygon of the input shapefile into independent
 *           polygon components (one outer ring plus its direct holes each),
 *           rewinding rings to the spec order, and writes the components to
 *           a new shapefile. Each output feature copies the attribute row of
 *           the physical source record identified by the source nShapeId, so
 *           one source row that splits into several components keeps the
 *           correct provenance for every output row.
 *
 *           Uses only Shapelib itself: no GIS SDK and no external data.
 *
 * Author:   Shapelib contributors
 *
 ******************************************************************************
 * SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
 ******************************************************************************
 */

#include "shapefil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: %s in_shapefile out_shapefile\n", argv[0]);
        return 1;
    }

    SHPHandle hIn = SHPOpen(argv[1], "rb");
    if (!hIn)
    {
        fprintf(stderr, "cannot open shapefile: %s\n", argv[1]);
        return 1;
    }
    DBFHandle hInDbf = DBFOpen(argv[1], "rb");
    if (!hInDbf)
    {
        fprintf(stderr, "cannot open dbf: %s\n", argv[1]);
        SHPClose(hIn);
        return 1;
    }

    int nEntities = 0;
    int nShapeType = 0;
    SHPGetInfo(hIn, &nEntities, &nShapeType, NULL, NULL);
    if (nShapeType != SHPT_POLYGON && nShapeType != SHPT_POLYGONZ &&
        nShapeType != SHPT_POLYGONM)
    {
        fprintf(stderr, "input shape type %d is not a polygon type\n",
                nShapeType);
        DBFClose(hInDbf);
        SHPClose(hIn);
        return 1;
    }

    SHPHandle hOut = SHPCreate(argv[2], nShapeType);
    if (!hOut)
    {
        fprintf(stderr, "cannot create shapefile: %s\n", argv[2]);
        DBFClose(hInDbf);
        SHPClose(hIn);
        return 1;
    }
    DBFHandle hOutDbf = DBFCloneEmpty(hInDbf, argv[2]);
    if (!hOutDbf)
    {
        fprintf(stderr, "cannot create dbf: %s\n", argv[2]);
        SHPClose(hOut);
        DBFClose(hInDbf);
        SHPClose(hIn);
        return 1;
    }

    /* Width of one raw DBF record, including the deletion flag byte. */
    int nRecordLength = 1;
    const int nFieldCount = DBFGetFieldCount(hInDbf);
    for (int iField = 0; iField < nFieldCount; iField++)
    {
        int nWidth = 0;
        int nDecimals = 0;
        if (DBFGetFieldInfo(hInDbf, iField, NULL, &nWidth, &nDecimals) ==
            FTInvalid)
            break;
        nRecordLength += nWidth;
    }

    int nOutEntities = 0;
    int nFailed = 0;
    for (int iShape = 0; iShape < nEntities; iShape++)
    {
        SHPObject *psShape = SHPReadObject(hIn, iShape);
        if (!psShape)
        {
            fprintf(stderr, "warning: cannot read shape %d, skipping\n",
                    iShape);
            nFailed++;
            continue;
        }

        SHPPolygonComponents *psComponents = NULL;
        char *pszError = NULL;
        if (!SHPBuildPolygonComponents(psShape, &psComponents, &pszError))
        {
            fprintf(stderr, "shape %d rejected: %s\n", iShape,
                    pszError ? pszError : "(no message)");
            free(pszError);
            SHPDestroyObject(psShape);
            nFailed++;
            continue;
        }

        for (int iComponent = 0; iComponent < psComponents->nComponents;
             iComponent++)
        {
            const SHPObject *psComponent =
                psComponents->pasComponents[iComponent].psObject;

            /* The component keeps the source shape id even when one      */
            /* source row explodes into several component rows.           */
            if (SHPWriteObject(hOut, -1, psComponent) < 0)
            {
                fprintf(stderr, "failed to write component %d of shape %d\n",
                        iComponent, iShape);
                nFailed++;
                continue;
            }

            /* Duplicate the physical attribute row of the source id. */
            const char *pachTuple =
                DBFReadTuple(hInDbf, psComponent->nShapeId);
            if (pachTuple)
            {
                char *pachCopy = malloc((size_t)nRecordLength);
                if (pachCopy)
                {
                    memcpy(pachCopy, pachTuple, (size_t)nRecordLength);
                    DBFWriteTuple(hOutDbf, nOutEntities, pachCopy);
                    free(pachCopy);
                }
            }
            nOutEntities++;
        }

        SHPDestroyPolygonComponents(psComponents);
        SHPDestroyObject(psShape);
    }

    SHPClose(hIn);
    DBFClose(hInDbf);
    SHPClose(hOut);
    DBFClose(hOutDbf);

    printf("wrote %d components from %d source shapes to %s", nOutEntities,
           nEntities, argv[2]);
    if (nFailed)
        printf(" (%d source shapes failed)", nFailed);
    printf("\n");
    return nFailed ? 1 : 0;
}
