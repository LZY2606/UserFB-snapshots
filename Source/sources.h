/*
** SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
** Copyright (C) [dates of first publication] Silicon Graphics, Inc.
** All Rights Reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
** of the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
**
** The above copyright including the dates of first publication and either this
** permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
** INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
** PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC.
** BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
** Per-half-edge bookkeeping of the original contour edges covering each
** piece of mesh geometry.  Records are created in tessAddContour() and
** follow the mesh through edge splits (parameter interval subdivision),
** coincident-edge merges and boundary extraction, so every original edge
** identity covering a final boundary edge can be reported, including
** overlapping or winding-cancelling contours.
*/

#ifndef SOURCES_H
#define SOURCES_H

#include "mesh.h"
#include "bucketalloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TESSsourceRec TESSsourceRec;

struct TESSsourceRec {
	TESSsourceRec *next;
	TESSindex contour;	/* contour number, 0-based in tessAddContour() order */
	TESSindex edge;		/* edge number within the contour */
	TESSreal t0, t1;	/* parameters on P(t) = P0 + t*(P1 - P0),
						** P(t0) reconstructs the half-edge origin, P(t1)
						** its destination; t0 > t1 means opposite direction */
};

struct TESSsourcePool {
	struct BucketAlloc *bucket;
};

/* Creates the pool backing source record allocations, or NULL on failure.
** While the TESS_BOUNDARY_SOURCES option is off, no pool is created and no
** source record is ever allocated. */
TESSsourcePool *tessSourceNewPool( TESSalloc *alloc );

/* Frees the pool and all the records backed by its buckets. */
void tessSourceDeletePool( TESSalloc *alloc, TESSsourcePool *pool );

/* Attaches the identity of an original contour edge with full parameter
** range [0,1] to a freshly created half-edge (the Sym half-edge gets the
** reversed range).  Returns 0 on allocation failure. */
int tessSourceSetContourEdge( TESSsourcePool *pool, TESShalfEdge *e,
							  TESSindex contour, TESSindex edge );

/* Mirrors tessMeshSplitEdge(): subdivides the parameter intervals of
** eOrg's records between eOrg and the new half-edge eOrg->Lnext.
** Returns 0 on allocation failure. */
int tessSourceSplit( TESSsourcePool *pool, TESShalfEdge *eOrg );

/* Moves every record from eDrop onto eKeep before eDrop is deleted as a
** geometrically coincident edge.  Duplicate source identities are merged.
** Returns 0 on allocation failure. */
int tessSourceMerge( TESSsourcePool *pool, TESShalfEdge *eKeep, TESShalfEdge *eDrop );

#ifdef __cplusplus
};
#endif

#endif /* SOURCES_H */
