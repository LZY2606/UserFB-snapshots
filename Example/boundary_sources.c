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
** Demonstrates the TESS_BOUNDARY_SOURCES option on two partially
** overlapping rectangles.  For every directed boundary edge it prints the
** original contour edges covering it together with their t0/t1 parameters,
** then reconstructs the edge endpoints from those parameters and checks the
** reconstruction against the actual tesselated coordinates.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "tesselator.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "boundary_sources example failed line %d: %s\n", __LINE__, #x); \
	return 1; } } while (0)

typedef struct {
	float x, y;
} Point;

static Point Eval(const Point *contour, int count, int edge, float t)
{
	Point p0 = contour[edge];
	Point p1 = contour[(edge + 1) % count];
	Point r;
	r.x = p0.x + t * (p1.x - p0.x);
	r.y = p0.y + t * (p1.y - p0.y);
	return r;
}

static int Near(float a, float b)
{
	return fabsf(a - b) <= 1e-5f;
}

int main(void)
{
	static const float normal[3] = {0, 0, 1};

	/* Two axis-aligned rectangles overlapping on x in [2,4]. */
	const Point rectA[4] = {{0,0},{4,0},{4,4},{0,4}};
	const Point rectB[4] = {{2,0},{6,0},{6,4},{2,4}};
	const Point *contours[2] = { rectA, rectB };
	const int counts[2] = { 4, 4 };

	TESStesselator *tess = tessNewTess(NULL);
	CHECK(tess != NULL);

	tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
	tessAddContour(tess, 2, rectA, 2 * sizeof(float), 4);
	tessAddContour(tess, 2, rectB, 2 * sizeof(float), 4);

	CHECK(tessTesselate(tess, TESS_WINDING_NONZERO,
	                    TESS_BOUNDARY_CONTOURS, 3, 2, normal));

	const TESSreal *verts = tessGetVertices(tess);
	const TESSindex *elems = tessGetElements(tess);
	const TESSindex *offsets = tessGetBoundarySourceOffsets(tess);
	const TESSboundarySource *sources = tessGetBoundarySources(tess);
	const int ncontours = tessGetElementCount(tess);
	const int nverts = tessGetVertexCount(tess);

	CHECK(offsets != NULL);
	CHECK(offsets[nverts] == tessGetBoundarySourceCount(tess));

	printf("boundary contours: %d, vertices: %d, source records: %d\n",
	       ncontours, nverts, tessGetBoundarySourceCount(tess));

	for (int c = 0; c < ncontours; ++c) {
		const int base = elems[2 * c];
		const int count = elems[2 * c + 1];
		for (int j = 0; j < count; ++j) {
			const int k = base + j;
			const int kn = base + (j + 1) % count;
			const float ax = verts[2 * k], ay = verts[2 * k + 1];
			const float bx = verts[2 * kn], by = verts[2 * kn + 1];

			printf("edge (%.1f,%.1f)->(%.1f,%.1f) sources:", ax, ay, bx, by);
			CHECK(offsets[k] <= offsets[k + 1]);
			for (TESSindex s = offsets[k]; s < offsets[k + 1]; ++s) {
				const TESSboundarySource *r = &sources[s];
				printf(" [contour %d edge %d t %.3f->%.3f]",
				       (int)r->contour, (int)r->edge, r->t0, r->t1);

				/* Independently reconstruct both endpoints from the
				** original contour and the reported parameters. */
				Point ra = Eval(contours[r->contour], counts[r->contour],
				                r->edge, r->t0);
				Point rb = Eval(contours[r->contour], counts[r->contour],
				                r->edge, r->t1);
				CHECK(Near(ra.x, ax) && Near(ra.y, ay));
				CHECK(Near(rb.x, bx) && Near(rb.y, by));
				CHECK(r->t0 >= 0.0f && r->t0 <= 1.0f);
				CHECK(r->t1 >= 0.0f && r->t1 <= 1.0f);
			}
			printf("\n");
		}
	}

	tessDeleteTess(tess);
	printf("BOUNDARY_SOURCE_EXAMPLE_OK\n");
	return 0;
}
