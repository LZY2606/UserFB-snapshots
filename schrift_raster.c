/* This file is part of libschrift.
 *
 * © 2019-2022 Thomas Oltmann and contributors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

/* Even-odd fill rule rasterizer.
 *
 * All closed contours are treated as one planar curve set and the filled
 * region is the set of points whose horizontal-ray crossing count is odd.
 * The output byte of a pixel is the area of the intersection of that region
 * with the pixel's unit square, times 255 and rounded.
 *
 * The integration is geometric and exact for the tessellated straight-edge
 * geometry, per pixel row:
 *
 *   1. Every edge crossing the row's horizontal band [Y, Y+1] is clipped to
 *      it, yielding a segment whose crossing abscissa is linear in y.
 *   2. The band is split at every in-band edge endpoint and at every
 *      intersection of two segment lines. Inside a resulting sub-band the
 *      left-to-right ordering of crossings never changes, so edges that
 *      cross inside a single pixel are handled geometrically instead of by
 *      XOR-ing per-contour coverage.
 *   3. At the sub-band midpoint, crossings are sorted and paired
 *      (0,1),(2,3),...; each pair bounds an odd-region trapezoid. Its
 *      intersection with each pixel column is integrated analytically by
 *      splitting the trapezoid at column-border crossings and summing the
 *      resulting piecewise-linear widths.
 *
 * Temporary storage is organized by contour edges plus the currently
 * scanned band. No per-pixel edge lists and no high-resolution proxy
 * bitmaps are created. */

#include "schrift_internal.h"

#include <math.h>
#include <stdlib.h>

/* Defined in schrift.c. */
int tesselate_curves(Outline *outl);
void transform_points(unsigned int numPts, Point *points, double trf[6]);

typedef struct
{
	double ax, ay, bx, by;
} EEdge;

typedef struct
{
	double p, q;   /* x(y) = p * y + q */
	double y0, y1; /* portion of the edge inside the current band */
} Segment;

typedef struct
{
	double x;
	int    seg;
} Crossing;

static int
cmp_crossing(const void *a, const void *b)
{
	const Crossing *ca = (const Crossing *) a;
	const Crossing *cb = (const Crossing *) b;
	if (ca->x < cb->x) return -1;
	if (ca->x > cb->x) return +1;
	return (ca->seg > cb->seg) - (ca->seg < cb->seg);
}

static int
cmp_double(const void *a, const void *b)
{
	double da = *(const double *) a, db = *(const double *) b;
	return (da > db) - (da < db);
}

/* Analytic area of the odd-region trapezoid between the two crossing lines
 * l(y)=p0*y+q0 and r(y)=p1*y+q1, over [ya,yb], intersected with pixel
 * column [c,c+1]. Both boundary lines are linear, so splitting [ya,yb] at
 * every column-border crossing and evaluating the clipped width at each
 * piece midpoint integrates the piecewise-linear width exactly. */
static double
column_integral(double p0, double q0, double p1, double q1,
                double ya, double yb, int c)
{
	double ys[16];
	int n = 0, i;
	double area = 0.0;
	ys[n++] = ya;
	ys[n++] = yb;
	{
		double ps[2] = { p0, p1 }, qs[2] = { q0, q1 };
		int s, b;
		for (s = 0; s < 2; ++s) {
			for (b = 0; b < 2; ++b) {
				double v = (double) c + (double) b;
				if (ps[s] != 0.0) {
					double y = (v - qs[s]) / ps[s];
					if (y > ya && y < yb)
						ys[n++] = y;
				}
			}
		}
	}
	qsort(ys, (size_t) n, sizeof *ys, cmp_double);
	for (i = 0; i + 1 < n; ++i) {
		double a = ys[i], bp = ys[i + 1], m;
		double xl, xr, lo, hi;
		if (bp <= a)
			continue;
		m = 0.5 * (a + bp);
		xl = p0 * m + q0;
		xr = p1 * m + q1;
		lo = xl > (double) c ? xl : (double) c;
		hi = xr < (double) c + 1.0 ? xr : (double) c + 1.0;
		if (hi > lo)
			area += (hi - lo) * (bp - a);
	}
	return area;
}

/* Adds the area of one odd-region trapezoid, bounded by the two crossing
 * lines s0 and s1 over [ya,yb], to every pixel column it overlaps. */
static void
add_trapezoid(double *areas, int width, const Segment *segs,
              int s0, int s1, double ya, double yb)
{
	double p0 = segs[s0].p, q0 = segs[s0].q;
	double p1 = segs[s1].p, q1 = segs[s1].q;
	double corners[4];
	double xmin, xmax;
	int c0, c1, c;

	corners[0] = p0 * ya + q0; corners[1] = p1 * ya + q1;
	corners[2] = p1 * yb + q1; corners[3] = p0 * yb + q0;

	xmin = fmin(fmin(corners[0], corners[1]), fmin(corners[2], corners[3]));
	xmax = fmax(fmax(corners[0], corners[1]), fmax(corners[2], corners[3]));
	if (xmax <= xmin || xmax <= 0.0 || xmin >= (double) width)
		return;
	/* Clamp to the column range before casting, so floor() results are always
	 * representable as int. */
	if (xmin < 0.0)
		xmin = 0.0;
	if (xmax > (double) width)
		xmax = (double) width;
	c0 = (int) floor(xmin);
	c1 = (int) floor(xmax);
	if (c1 >= width)
		c1 = width - 1;
	for (c = c0; c <= c1; ++c) {
		double a = column_integral(p0, q0, p1, q1, ya, yb, c);
		if (a > 0.0)
			areas[c] += a;
	}
}

static int
push_event(double **buf, size_t *num, size_t *cap, double y)
{
	if (*num == *cap) {
		size_t ncap = *cap ? *cap * 2 : 32;
		double *mem = (double *) realloc(*buf, ncap * sizeof *mem);
		if (!mem)
			return -1;
		*buf = mem;
		*cap = ncap;
	}
	(*buf)[(*num)++] = y;
	return 0;
}

static int
render_band(const EEdge *edges, size_t numEdges, int row, int width,
            double *areas, Segment *segBuf, size_t segCap,
            Crossing *crossBuf, double **yBuf, size_t *yCap)
{
	double lo = (double) row, hi = lo + 1.0;
	size_t numSeg = 0, numY = 0;
	size_t i, j, k;

	for (i = 0; i < numEdges; ++i) {
		double ay = edges[i].ay, by = edges[i].by, y0, y1;
		if (ay == by)
			continue; /* horizontal edges only touch band boundaries */
		y0 = fmax(lo, fmin(ay, by));
		y1 = fmin(hi, fmax(ay, by));
		if (y1 <= lo || y0 >= hi)
			continue;
		if (numSeg >= segCap)
			return -1;
		{
			double dx = edges[i].bx - edges[i].ax;
			double dy = by - ay;
			double p = dx / dy;
			segBuf[numSeg].p = p;
			segBuf[numSeg].q = edges[i].ax - p * ay;
			segBuf[numSeg].y0 = y0;
			segBuf[numSeg].y1 = y1;
			++numSeg;
		}
	}
	if (!numSeg)
		return 0;

	if (push_event(yBuf, &numY, yCap, lo) < 0 ||
	    push_event(yBuf, &numY, yCap, hi) < 0)
		return -1;
	/* In-band endpoints ... */
	for (i = 0; i < numSeg; ++i) {
		if (segBuf[i].y0 > lo && segBuf[i].y0 < hi)
			if (push_event(yBuf, &numY, yCap, segBuf[i].y0) < 0)
				return -1;
		if (segBuf[i].y1 > lo && segBuf[i].y1 < hi)
			if (push_event(yBuf, &numY, yCap, segBuf[i].y1) < 0)
				return -1;
	}
	/* ... and in-band intersections of segment lines. */
	for (i = 0; i < numSeg; ++i) {
		for (j = i + 1; j < numSeg; ++j) {
			double dp = segBuf[i].p - segBuf[j].p;
			double dq = segBuf[j].q - segBuf[i].q;
			double y;
			if (dp == 0.0)
				continue; /* parallel: coincident lines stay tied, others
				           * never meet */
			y = dq / dp;
			if (y > lo && y < hi)
				if (push_event(yBuf, &numY, yCap, y) < 0)
					return -1;
		}
	}
	qsort(*yBuf, numY, sizeof **yBuf, cmp_double);

	for (k = 0; k + 1 < numY; ++k) {
		double ya = (*yBuf)[k], yb = (*yBuf)[k + 1], ym;
		size_t nc;
		if (yb <= ya)
			continue;
		ym = 0.5 * (ya + yb);
		nc = 0;
		for (i = 0; i < numSeg; ++i) {
			if (segBuf[i].y0 <= ym && segBuf[i].y1 >= ym) {
				crossBuf[nc].x = segBuf[i].p * ym + segBuf[i].q;
				crossBuf[nc].seg = (int) i;
				++nc;
			}
		}
		qsort(crossBuf, nc, sizeof *crossBuf, cmp_crossing);
		/* Pair crossings: [0,1], [2,3], ... are the odd-region intervals. */
		for (i = 0; i + 1 < nc; i += 2) {
			add_trapezoid(areas, width, segBuf,
			              crossBuf[i].seg, crossBuf[i + 1].seg, ya, yb);
		}
	}
	return 0;
}

int
sft_render_even_odd(Outline *outl, double transform[6], SFT_Image image)
{
	EEdge *edges = NULL;
	double *areas = NULL;
	Segment *segBuf = NULL;
	Crossing *crossBuf = NULL;
	double *yBuf = NULL;
	size_t numEdges = 0, capEdges = 0;
	size_t segCap, crossCap, yCap;
	unsigned int i;
	int width = image.width, height = image.height;
	int row;

	if (width <= 0 || height <= 0 || !image.pixels)
		return -1;
	if (!isfinite((double) width) || (size_t) width > (size_t) -1 / (size_t) height)
		return -1;

	/* Transform first and tessellate on the unclipped, transformed outline so
	 * control points outside the crop window keep their true positions. */
	transform_points(outl->numPoints, outl->points, transform);
	if (tesselate_curves(outl) < 0)
		return -1;

	for (i = 0; i < outl->numLines; ++i) {
		Line line = outl->lines[i];
		Point a = outl->points[line.beg];
		Point b = outl->points[line.end];
		if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(b.x) || !isfinite(b.y))
			goto failure;
		if (numEdges == capEdges) {
			size_t ncap = capEdges ? capEdges * 2 : 64;
			EEdge *mem = (EEdge *) realloc(edges, ncap * sizeof *mem);
			if (!mem)
				goto failure;
			edges = mem;
			capEdges = ncap;
		}
		edges[numEdges++] = (EEdge) { a.x, a.y, b.x, b.y };
	}

	areas = (double *) calloc((size_t) width * (size_t) height, sizeof *areas);
	if (!areas)
		goto failure;

	/* Per-band scratch storage, reused across rows. The event buffer starts
	 * small and grows on demand, so a glyph with many edges never forces an
	 * edges-squared allocation up front. */
	segCap = numEdges + 1;
	crossCap = numEdges + 1;
	segBuf = (Segment *) malloc(segCap * sizeof *segBuf);
	crossBuf = (Crossing *) malloc(crossCap * sizeof *crossBuf);
	if (!segBuf || !crossBuf)
		goto failure;
	yCap = 0; /* yBuf grows via push_event() inside render_band(). */

	for (row = 0; row < height; ++row) {
		double *rowAreas = areas + (size_t) row * (size_t) width;
		if (render_band(edges, numEdges, row, width, rowAreas,
		                segBuf, segCap, crossBuf, &yBuf, &yCap) < 0)
			goto failure;
	}

	/* Commit only after the whole image has been integrated successfully. */
	{
		size_t total = (size_t) width * (size_t) height;
		unsigned char *out = (unsigned char *) image.pixels;
		size_t k;
		for (k = 0; k < total; ++k) {
			double a = areas[k];
			if (a < 0.0)
				a = 0.0;
			else if (a > 1.0)
				a = 1.0;
			out[k] = (unsigned char) (a * 255.0 + 0.5);
		}
	}

	free(yBuf);
	free(crossBuf);
	free(segBuf);
	free(areas);
	free(edges);
	return 0;

failure:
	free(yBuf);
	free(crossBuf);
	free(segBuf);
	free(areas);
	free(edges);
	return -1;
}
