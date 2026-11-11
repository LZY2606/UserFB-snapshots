// Tests for nvgStrokeProfile().
//
// The tests drive the public API and inspect the exact vertices handed to
// the renderStroke callback, plus the callback metadata (paint, alpha,
// scissor and composite operation). No window or GL context is needed.

#include "nanovg.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATHS 16
#define MAX_VERTS 16384

typedef struct Capture {
	int strokes;
	int fills;
	int triangles;
	NVGpaint paint;
	NVGcompositeOperationState composite;
	NVGscissor scissor;
	float fringe;
	float strokeWidth;
	int npaths;
	int pathStart[MAX_PATHS];
	int pathNStroke[MAX_PATHS];
	int pathClosed[MAX_PATHS];
	int pathFirst[MAX_PATHS];
	int pathCount[MAX_PATHS];
	int nverts;
	NVGvertex verts[MAX_VERTS];
	// Compact fill summary, used to prove profile drawing does not disturb
	// the shared path cache.
	int fillNPaths;
	int fillNVert;
	double fillChecksum;
} Capture;

static void capReset(Capture* c)
{
	memset(c, 0, sizeof(*c));
}

static int renderCreate(void* p) { (void)p; return 1; }
static int renderCreateTexture(void* p, int t, int w, int h, int f, const unsigned char* d)
{ (void)p; (void)t; (void)w; (void)h; (void)f; (void)d; return 1; }
static int renderDeleteTexture(void* p, int i) { (void)p; (void)i; return 1; }
static void renderViewport(void* p, float w, float h, float d) { (void)p; (void)w; (void)h; (void)d; }
static void renderNoop(void* p) { (void)p; }

static void renderFill(void* p, NVGpaint* paint, NVGcompositeOperationState comp, NVGscissor* sc,
					   float fringe, const float* bounds, const NVGpath* paths, int npaths)
{
	Capture* c = (Capture*)p;
	int i, j;
	(void)paint; (void)comp; (void)sc; (void)fringe; (void)bounds;
	c->fills++;
	c->fillNPaths = npaths;
	c->fillNVert = 0;
	c->fillChecksum = 0.0;
	for (i = 0; i < npaths; i++) {
		for (j = 0; j < paths[i].nfill; j++) {
			c->fillNVert++;
			c->fillChecksum += (double)paths[i].fill[j].x + 3.0*(double)paths[i].fill[j].y;
		}
	}
}

static void renderStroke(void* p, NVGpaint* paint, NVGcompositeOperationState comp, NVGscissor* sc,
						 float fringe, float strokeWidth, const NVGpath* paths, int npaths)
{
	Capture* c = (Capture*)p;
	int i, j, k = 0;

	c->strokes++;
	c->paint = *paint;
	c->composite = comp;
	c->scissor = *sc;
	c->fringe = fringe;
	c->strokeWidth = strokeWidth;
	c->npaths = npaths;

	for (i = 0; i < npaths && i < MAX_PATHS; i++) {
		const NVGpath* path = &paths[i];
		c->pathStart[i] = k;
		c->pathNStroke[i] = path->nstroke;
		c->pathClosed[i] = path->closed;
		c->pathFirst[i] = path->first;
		c->pathCount[i] = path->count;
		for (j = 0; j < path->nstroke && k < MAX_VERTS; j++)
			c->verts[k++] = path->stroke[j];
	}
	c->nverts = k;
}

static void renderTriangles(void* p, NVGpaint* paint, NVGcompositeOperationState comp, NVGscissor* sc,
							const NVGvertex* verts, int nverts, float fringe)
{
	Capture* c = (Capture*)p;
	(void)paint; (void)comp; (void)sc; (void)verts; (void)nverts; (void)fringe;
	c->triangles++;
}

static NVGcontext* makeContext(Capture* c, int edgeAA)
{
	NVGparams p;
	memset(&p, 0, sizeof(p));
	p.userPtr = c;
	p.edgeAntiAlias = edgeAA;
	p.renderCreate = renderCreate;
	p.renderCreateTexture = renderCreateTexture;
	p.renderDeleteTexture = renderDeleteTexture;
	p.renderViewport = renderViewport;
	p.renderCancel = renderNoop;
	p.renderFlush = renderNoop;
	p.renderFill = renderFill;
	p.renderStroke = renderStroke;
	p.renderTriangles = renderTriangles;
	p.renderDelete = renderNoop;
	return nvgCreateInternal(&p);
}

static void frame(NVGcontext* ctx, float dpr)
{
	nvgBeginFrame(ctx, 256, 256, dpr);
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabsf((a)-(b)) <= (eps))

static float vertexDist(const NVGvertex* v, float x, float y)
{
	float dx = v->x - x;
	float dy = v->y - y;
	return sqrtf(dx*dx + dy*dy);
}

static int findVertex(const Capture* c, float x, float y, float eps)
{
	int i;
	for (i = 0; i < c->nverts; i++) {
		if (vertexDist(&c->verts[i], x, y) <= eps)
			return 1;
	}
	return 0;
}

static void boundsAtX(const Capture* c, float x, float xeps, float* ymin, float* ymax)
{
	int i;
	int found = 0;
	*ymin = 1e30f;
	*ymax = -1e30f;
	for (i = 0; i < c->nverts; i++) {
		if (fabsf(c->verts[i].x - x) <= xeps) {
			if (c->verts[i].y < *ymin) *ymin = c->verts[i].y;
			if (c->verts[i].y > *ymax) *ymax = c->verts[i].y;
			found = 1;
		}
	}
	if (!found)
		fprintf(stderr, "boundsAtX: x=%g nverts=%d\n", x, c->nverts);
	CHECK(found);
}

static float minVertexDistanceNear(const Capture* c, float x, float y, float radius)
{
	int i;
	float best = 1e30f;
	for (i = 0; i < c->nverts; i++) {
		float d = vertexDist(&c->verts[i], x, y);
		if (d > 1e-4f && d <= radius && d < best)
			best = d;
	}
	if (!(best < 1e29f))
		fprintf(stderr, "minVertexDistanceNear: (%g,%g) radius %g nverts=%d\n", x, y, radius, c->nverts);
	CHECK(best < 1e29f);
	return best;
}

static int allFinite(const Capture* c)
{
	int i;
	for (i = 0; i < c->nverts; i++) {
		if (!isfinite(c->verts[i].x) || !isfinite(c->verts[i].y))
			return 0;
	}
	return 1;
}

static void beginLine(NVGcontext* ctx, float x0, float y0, float x1, float y1)
{
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, x0, y0);
	nvgLineTo(ctx, x1, y1);
}

// --------------------------------------------------------------------------
// Straight gradient is a trapezoid, and a mid-segment stop creates a kink.
// --------------------------------------------------------------------------
static void testStraightGradient(void)
{
	Capture cap;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 8.0f}, {1.0f, 12.0f} };
	NVGwidthStop kink[] = { {0.0f, 4.0f}, {0.25f, 2.0f}, {1.0f, 12.0f} };
	float ymin, ymax;

	capReset(&cap);
	ctx = makeContext(&cap, 0);
	frame(ctx, 1.0f);
	nvgStrokeColor(ctx, nvgRGBAf(0, 0, 0, 1));
	nvgLineCap(ctx, NVG_BUTT);

	beginLine(ctx, 0, 0, 200, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(cap.strokes == 1);
	CHECK(cap.npaths == 1);
	CHECK(cap.pathClosed[0] == 0);
	CHECK(cap.pathNStroke[0] > 0);

	boundsAtX(&cap, 0.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -2.0f, 1e-3f);
	CHECK_NEAR(ymax,  2.0f, 1e-3f);

	boundsAtX(&cap, 100.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -4.0f, 1e-3f);
	CHECK_NEAR(ymax,  4.0f, 1e-3f);

	boundsAtX(&cap, 200.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -6.0f, 1e-3f);
	CHECK_NEAR(ymax,  6.0f, 1e-3f);

	// The sides really are trapezoid edges: midpoint of the first span is
	// the linear interpolation of its endpoint widths.
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 200, 0);
	capReset(&cap);
	CHECK(nvgStrokeProfile(ctx, kink, 3) == 1);
	CHECK(cap.strokes == 1);
	boundsAtX(&cap, 0.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -2.0f, 1e-3f);
	CHECK_NEAR(ymax,  2.0f, 1e-3f);
	boundsAtX(&cap, 50.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -1.0f, 1e-3f);
	CHECK_NEAR(ymax,  1.0f, 1e-3f);
	boundsAtX(&cap, 200.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -6.0f, 1e-3f);
	CHECK_NEAR(ymax,  6.0f, 1e-3f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Miter tip must be the intersection of the two gradient side lines, not an
// equal-width corner vertex scaled by a width.
// --------------------------------------------------------------------------
static void testMiterIntersection(void)
{
	Capture cap;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {1.0f, 12.0f} };
	// 200px east then 200px south. At the corner w = 8, h = 4.
	// plus side of segment 1: y = -2 - 0.01*x
	// plus side of segment 2: x = 204 + 0.01*y
	// intersection: (203.9596..., -4.0396...)
	const float ex = 203.9596f;
	const float ey = -4.0396f;

	capReset(&cap);
	ctx = makeContext(&cap, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	nvgLineJoin(ctx, NVG_MITER);
	nvgMiterLimit(ctx, 10.0f);

	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 200, 0);
	nvgLineTo(ctx, 200, 200);
	CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
	CHECK(cap.strokes == 1);
	CHECK(cap.npaths == 1);
	CHECK(allFinite(&cap));
	CHECK(findVertex(&cap, ex, ey, 0.03f));
	// The naive equal-width miter would be (204,-4).
	CHECK(!findVertex(&cap, 204.0f, -4.0f, 0.01f));

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Bevel and round joins on a sharp turn are supported and stay finite.
// --------------------------------------------------------------------------
static void testJoinsAndMiterLimit(void)
{
	Capture cap;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {1.0f, 12.0f} };
	int join;

	for (join = 0; join < 3; join++) {
		capReset(&cap);
		ctx = makeContext(&cap, 0);
		frame(ctx, 1.0f);
		nvgLineCap(ctx, NVG_BUTT);
		if (join == 0) {
			nvgLineJoin(ctx, NVG_MITER);
			nvgMiterLimit(ctx, 1.5f);
		} else if (join == 1) {
			nvgLineJoin(ctx, NVG_BEVEL);
		} else {
			nvgLineJoin(ctx, NVG_ROUND);
		}

		nvgBeginPath(ctx);
		nvgMoveTo(ctx, 0, 0);
		nvgLineTo(ctx, 200, 0);
		nvgLineTo(ctx, 0, 200); // 135 degree turn
		CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
		CHECK(cap.strokes == 1);
		CHECK(allFinite(&cap));

		// The miter limit is measured against the local half width; with a
		// limit of 1.5 this corner must not emit the far miter spike.
		if (join == 0) {
			float maxd = 0.0f;
			int i;
			for (i = 0; i < cap.nverts; i++) {
				float d = vertexDist(&cap.verts[i], 200, 0);
				if (d <= 20.0f && d > maxd) maxd = d;
			}
			CHECK(maxd > 1.0f);
			CHECK(maxd < 15.0f);
		}
		nvgEndFrame(ctx);
		nvgDeleteInternal(ctx);
	}

	// Round join arc at a 90 degree corner, local half width 4.
	capReset(&cap);
	ctx = makeContext(&cap, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	nvgLineJoin(ctx, NVG_ROUND);
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 200, 0);
	nvgLineTo(ctx, 200, 200);
	CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
	CHECK(allFinite(&cap));
	// The outer bisector direction is (1,-1)/sqrt(2), radius 4.
	CHECK(findVertex(&cap, 200 + 4*0.70710678f, -4*0.70710678f, 0.05f));

	// The mirrored turn puts the outer side on the other normal side.
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 200, 0);
	nvgLineTo(ctx, 200, -200);
	capReset(&cap);
	CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
	CHECK(allFinite(&cap));
	CHECK(findVertex(&cap, 200 + 4*0.70710678f, 4*0.70710678f, 0.05f));
	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Closed seam: one real join, no duplicated caps at the start.
// --------------------------------------------------------------------------
static void testClosedSeam(void)
{
	Capture cap;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 10.0f}, {1.0f, 4.0f} };
	int i;

	capReset(&cap);
	ctx = makeContext(&cap, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_ROUND); // must not be used on a closed sub-path
	nvgLineJoin(ctx, NVG_MITER);
	nvgMiterLimit(ctx, 10.0f);

	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 200, 0);
	nvgLineTo(ctx, 200, 200);
	nvgLineTo(ctx, 0, 200);
	nvgClosePath(ctx);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(cap.strokes == 1);
	CHECK(cap.npaths == 1);
	CHECK(cap.pathClosed[0] == 1);
	CHECK(allFinite(&cap));

	// Seam width is 4 (h=2). The seam join's miter points for the incoming
	// (0,-1) and outgoing (1,0) directions are (-2,-2) and (2,2).
	CHECK(findVertex(&cap, -2.0f, -2.0f, 0.05f));
	CHECK(findVertex(&cap, 2.0f, 2.0f, 0.05f));
	// A round start cap would emit its fan centre at the path start.
	CHECK(!findVertex(&cap, 0.0f, 0.0f, 1e-4f));
	// No butt-cap pair at the seam either.
	CHECK(!findVertex(&cap, 0.0f, -2.0f, 1e-3f) || !findVertex(&cap, 0.0f, 2.0f, 1e-3f));

	// Widths at the other corners follow the profile: t=0.25 -> 7, t=0.5
	// -> 10, t=0.75 -> 7. Miter joins put the tip at h*sqrt(2) for 90 deg.
	CHECK_NEAR(minVertexDistanceNear(&cap, 200, 0, 12), 3.5f*1.41421356f, 0.25f);
	CHECK_NEAR(minVertexDistanceNear(&cap, 200, 200, 12), 5.0f*1.41421356f, 0.25f);
	CHECK_NEAR(minVertexDistanceNear(&cap, 0, 200, 12), 3.5f*1.41421356f, 0.25f);

	// The closed strip is closed: the final pair repeats the first pair.
	CHECK(cap.pathNStroke[0] >= 4);
	for (i = 0; i < cap.nverts; i++)
		CHECK(isfinite(cap.verts[i].x) && isfinite(cap.verts[i].y));

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// t follows the path's input direction even after fill flattened/reversed
// the shared path cache for winding.
// --------------------------------------------------------------------------
static void testReverseWinding(void)
{
	Capture cap;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 2.0f}, {0.5f, 10.0f}, {1.0f, 2.0f} };

	capReset(&cap);
	ctx = makeContext(&cap, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	nvgLineJoin(ctx, NVG_MITER);

	// Negative-area input order. A normal CCW fill reverses this point list
	// in the cache.
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 0, 200);
	nvgLineTo(ctx, 200, 200);
	nvgLineTo(ctx, 200, 0);
	nvgClosePath(ctx);

	// Force the ordinary fill preparation (and its winding reversal).
	nvgFillColor(ctx, nvgRGBAf(0, 0, 0, 1));
	nvgFill(ctx);
	CHECK(cap.fills == 1);

	capReset(&cap);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(cap.strokes == 1);
	CHECK(cap.npaths == 1);
	CHECK(cap.pathClosed[0] == 1);

	// Input order: start (0,0), t=.25 (0,200), t=.5 (200,200), t=.75
	// (200,0). So the thick end is at (200,200), not (0,200).
	CHECK_NEAR(minVertexDistanceNear(&cap, 200, 200, 12), 5.0f*1.41421356f, 0.25f);
	CHECK_NEAR(minVertexDistanceNear(&cap, 0, 200, 12), 3.0f*1.41421356f, 0.25f);
	CHECK_NEAR(minVertexDistanceNear(&cap, 200, 0, 12), 3.0f*1.41421356f, 0.25f);
	CHECK_NEAR(minVertexDistanceNear(&cap, 0, 0, 12), 1.0f*1.41421356f, 0.25f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Constant profile is exactly an ordinary stroke, and the path remains
// usable for fill / ordinary stroke / another profile afterwards.
// --------------------------------------------------------------------------
static void testConstantAndReuse(void)
{
	Capture a, b;
	NVGcontext* ctx;
	NVGwidthStop constant[] = { {0.0f, 6.0f}, {0.35f, 6.0f}, {1.0f, 6.0f} };
	NVGwidthStop profileA[] = { {0.0f, 2.0f}, {1.0f, 10.0f} };
	NVGwidthStop profileB[] = { {0.0f, 6.0f}, {1.0f, 4.0f} };
	int i;

	capReset(&a);
	ctx = makeContext(&a, 0);
	frame(ctx, 1.0f);
	nvgStrokeColor(ctx, nvgRGBAf(0, 0, 0, 1));
	nvgStrokeWidth(ctx, 6.0f);
	nvgLineJoin(ctx, NVG_ROUND);

	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 10, 10);
	nvgBezierTo(ctx, 20, 40, 40, 0, 60, 30);
	nvgLineTo(ctx, 90, 10);
	nvgStroke(ctx);
	CHECK(a.strokes == 1);
	CHECK_NEAR(a.strokeWidth, 6.0f, 1e-5f);

	capReset(&b);
	nvgInternalParams(ctx)->userPtr = &b;
	CHECK(nvgStrokeProfile(ctx, constant, 3) == 1);
	CHECK(b.strokes == 1);
	CHECK(b.npaths == a.npaths);
	CHECK_NEAR(b.strokeWidth, 6.0f, 1e-5f);
	CHECK(b.nverts == a.nverts);
	for (i = 0; i < a.nverts; i++) {
		CHECK_NEAR(a.verts[i].x, b.verts[i].x, 1e-5f);
		CHECK_NEAR(a.verts[i].y, b.verts[i].y, 1e-5f);
		CHECK_NEAR(a.verts[i].u, b.verts[i].u, 1e-5f);
		CHECK_NEAR(a.verts[i].v, b.verts[i].v, 1e-5f);
	}
	CHECK(b.pathNStroke[0] == a.pathNStroke[0]);

	// Ordinary stroke still uses the state width and produces the same
	// geometry as it did before the profile draw.
	capReset(&b);
	nvgStroke(ctx);
	CHECK(b.strokes == 1);
	CHECK_NEAR(b.strokeWidth, 6.0f, 1e-5f);
	CHECK(b.npaths == 1);
	CHECK(b.nverts == a.nverts);
	for (i = 0; i < a.nverts; i++) {
		CHECK_NEAR(a.verts[i].x, b.verts[i].x, 1e-5f);
		CHECK_NEAR(a.verts[i].y, b.verts[i].y, 1e-5f);
		CHECK_NEAR(a.verts[i].u, b.verts[i].u, 1e-5f);
		CHECK_NEAR(a.verts[i].v, b.verts[i].v, 1e-5f);
	}

	// Another profile still follows the path.
	capReset(&b);
	CHECK(nvgStrokeProfile(ctx, profileA, 2) == 1);
	CHECK(b.strokes == 1);
	CHECK(b.npaths == 1);
	CHECK(b.nverts > 0);

	// And fill still works on the same path.
	capReset(&b);
	nvgFillColor(ctx, nvgRGBAf(1, 0, 0, 1));
	nvgFill(ctx);
	CHECK(b.fills == 1);

	// Leave the path ready for one more profile.
	capReset(&b);
	CHECK(nvgStrokeProfile(ctx, profileB, 2) == 1);
	CHECK(b.strokes == 1);
	CHECK(b.npaths == 1);

	// Caller's control points are never modified.
	{
		NVGwidthStop copy[3];
		memcpy(copy, profileA, sizeof(profileA));
		capReset(&b);
		CHECK(nvgStrokeProfile(ctx, profileB, 2) == 1);
		CHECK(memcmp(copy, profileA, sizeof(profileA)) == 0);
	}

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Caps use the width at their own endpoint; joins use local widths.
// --------------------------------------------------------------------------
static void testCaps(void)
{
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 6.0f}, {1.0f, 8.0f} };
	int cap;

	for (cap = 0; cap < 3; cap++) {
		Capture c;
		NVGcontext* ctx;
		capReset(&c);
		ctx = makeContext(&c, 0);
		frame(ctx, 1.0f);
		nvgLineJoin(ctx, NVG_MITER);
		nvgLineCap(ctx, cap == 0 ? NVG_BUTT : (cap == 1 ? NVG_ROUND : NVG_SQUARE));

		nvgBeginPath(ctx);
		nvgMoveTo(ctx, 0, 0);
		nvgLineTo(ctx, 100, 0);
		CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
		CHECK(c.strokes == 1);
		CHECK(allFinite(&c));

		if (cap == NVG_BUTT) {
			CHECK(findVertex(&c, 0.0f, -2.0f, 1e-3f));
			CHECK(findVertex(&c, 100.0f, 4.0f, 1e-3f));
			CHECK(!findVertex(&c, -2.0f, 0.0f, 0.05f));
		} else if (cap == NVG_SQUARE) {
			CHECK(findVertex(&c, -2.0f, -2.0f, 1e-3f));
			CHECK(findVertex(&c, 104.0f, 4.0f, 1e-3f));
		} else {
			// Round caps arc from (0,+h) through the semicircle behind the
			// endpoint; the exact sampled angles depend on tessTol.
			CHECK(findVertex(&c, 0.0f, -2.0f, 1e-3f));
			CHECK(findVertex(&c, 0.0f, 2.0f, 1e-3f));
			CHECK(findVertex(&c, 104.0f, 0.0f, 1e-3f));
		}
		nvgEndFrame(ctx);
		nvgDeleteInternal(ctx);
	}
}

// --------------------------------------------------------------------------
// Antialiasing: fringe metadata stays as before; sub-pixel regions carry
// their local coverage instead of a whole-stroke max-width decision.
// --------------------------------------------------------------------------
static void testAntialiasAndLocalThinWidth(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 20.0f}, {0.5f, 0.5f}, {1.0f, 20.0f} };
	float ymin, ymax;
	int i;
	float vmin = 2.0f;
	int sawThin = 0;

	capReset(&c);
	ctx = makeContext(&c, 1);
	frame(ctx, 1.0f);
	nvgLineJoin(ctx, NVG_MITER);
	nvgLineCap(ctx, NVG_BUTT);
	nvgStrokeColor(ctx, nvgRGBAf(0, 0, 0, 1));

	beginLine(ctx, 0, 0, 100, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.strokes == 1);
	CHECK_NEAR(c.fringe, 1.0f, 1e-6f);
	CHECK(c.paint.innerColor.a <= 1.000001f);
	CHECK(c.paint.innerColor.a >= 0.999f);

	// Fringe coordinates stay in the existing [0,1] convention.
	for (i = 0; i < c.nverts; i++) {
		CHECK(c.verts[i].u >= -1e-6f && c.verts[i].u <= 1.0f + 1e-6f);
		CHECK(c.verts[i].v >= -1e-6f && c.verts[i].v <= 1.0f + 1e-6f);
	}

	// At the thin station the geometry uses the local (widened-to-fringe)
	// width, not the maximum width.
	boundsAtX(&c, 50.0f, 1e-3f, &ymin, &ymax);
	CHECK(fabsf(ymin) <= 1.001f);
	CHECK(fabsf(ymax) <= 1.001f);

	for (i = 0; i < c.nverts; i++) {
		if (fabsf(c.verts[i].x - 50.0f) <= 1e-3f) {
			sawThin = 1;
			if (c.verts[i].v < vmin) vmin = c.verts[i].v;
		}
	}
	CHECK(sawThin);
	CHECK(vmin <= 0.30f);       // (0.5 / 1.0)^2 = 0.25
	CHECK(vmin >= 0.20f);

	// A thick station is fully covered. (Butt caps shift their solid pair
	// by fringe/2 when AA is enabled.)
	boundsAtX(&c, 99.5f, 1e-3f, &ymin, &ymax);
	CHECK(fabsf(ymax) > 5.0f);  // half width ~= 10 + fringe/2
	{
		float vmax = 0.0f;
		for (i = 0; i < c.nverts; i++) {
			if (fabsf(c.verts[i].x - 99.5f) <= 1e-3f && c.verts[i].v > vmax)
				vmax = c.verts[i].v;
		}
		CHECK(vmax >= 0.99f);
	}

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);

	// With AA off, the same thin region stays exactly at its local width.
	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	beginLine(ctx, 0, 0, 100, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	boundsAtX(&c, 50.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -0.25f, 1e-3f);
	CHECK_NEAR(ymax,  0.25f, 1e-3f);
	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Device pixel ratio affects the fringe convention, not the canvas-space
// profile length/width math.
// --------------------------------------------------------------------------
static void testDevicePixelRatio(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 8.0f}, {1.0f, 12.0f} };
	float ymin, ymax;

	capReset(&c);
	ctx = makeContext(&c, 1);
	frame(ctx, 2.0f);
	nvgLineCap(ctx, NVG_BUTT);
	beginLine(ctx, 0, 0, 200, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.strokes == 1);
	CHECK_NEAR(c.fringe, 0.5f, 1e-6f);

	// Canvas coordinates and widths are unchanged by DPR. The AA fringe
	// offsets the solid butt-cap pair by fringe/2.
	boundsAtX(&c, 0.25f, 1e-3f, &ymin, &ymax);
	CHECK(ymin < -1.5f && ymax > 1.5f);
	boundsAtX(&c, 100.0f, 1e-3f, &ymin, &ymax);
	CHECK(ymin < -2.0f && ymax > 2.0f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Callback metadata: color/global alpha/scissor/composite operation.
// --------------------------------------------------------------------------
static void testCallbackState(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {1.0f, 12.0f} };

	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);
	nvgStrokeColor(ctx, nvgRGBAf(0.2f, 0.4f, 0.6f, 0.8f));
	nvgGlobalAlpha(ctx, 0.5f);
	nvgGlobalCompositeOperation(ctx, NVG_LIGHTER);
	nvgScissor(ctx, 10, 20, 100, 50);
	nvgLineCap(ctx, NVG_BUTT);

	beginLine(ctx, 0, 0, 200, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
	CHECK(c.strokes == 1);
	CHECK_NEAR(c.paint.innerColor.r, 0.2f, 1e-5f);
	CHECK_NEAR(c.paint.innerColor.g, 0.4f, 1e-5f);
	CHECK_NEAR(c.paint.innerColor.b, 0.6f, 1e-5f);
	CHECK_NEAR(c.paint.innerColor.a, 0.4f, 1e-5f);
	CHECK(c.composite.srcRGB == NVG_ONE);
	CHECK(c.composite.dstRGB == NVG_ONE);
	CHECK_NEAR(c.scissor.extent[0], 50.0f, 1e-5f);
	CHECK_NEAR(c.scissor.extent[1], 25.0f, 1e-5f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Empty paths and zero-length sub-paths produce no geometry but are valid.
// --------------------------------------------------------------------------
static void testEmptyAndDegenerate(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 6.0f}, {1.0f, 8.0f} };

	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);

	nvgBeginPath(ctx);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.strokes == 1);
	CHECK(c.npaths == 0);
	CHECK(c.nverts == 0);

	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 10, 10);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.strokes == 2);
	CHECK(c.npaths == 0);

	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 10, 10);
	nvgLineTo(ctx, 10, 10);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.npaths == 0);

	// One usable sub-path plus one zero-length sub-path.
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 100, 0);
	nvgMoveTo(ctx, 30, 30);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.npaths == 1);
	CHECK(c.pathNStroke[0] > 0);
	CHECK(c.pathClosed[0] == 0);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Profile validation: invalid input returns 0 and submits no draw call.
// --------------------------------------------------------------------------
static void testValidation(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop ok[] = { {0.0f, 4.0f}, {0.5f, 8.0f}, {1.0f, 12.0f} };
	NVGwidthStop one[] = { {0.0f, 4.0f} };
	NVGwidthStop badFirst[] = { {0.1f, 4.0f}, {1.0f, 8.0f} };
	NVGwidthStop badLast[] = { {0.0f, 4.0f}, {0.9f, 8.0f} };
	NVGwidthStop badOrder[] = { {0.0f, 4.0f}, {0.5f, 8.0f}, {0.5f, 10.0f}, {1.0f, 12.0f} };
	NVGwidthStop badNaN[] = { {0.0f, 4.0f}, {NAN, 8.0f}, {1.0f, 12.0f} };
	NVGwidthStop badInfT[] = { {0.0f, 4.0f}, {INFINITY, 8.0f}, {1.0f, 12.0f} };
	NVGwidthStop badZero[] = { {0.0f, 4.0f}, {0.5f, 0.0f}, {1.0f, 12.0f} };
	NVGwidthStop badNeg[] = { {0.0f, 4.0f}, {0.5f, -2.0f}, {1.0f, 12.0f} };
	NVGwidthStop badNaNw[] = { {0.0f, 4.0f}, {0.5f, NAN}, {1.0f, 12.0f} };
	NVGwidthStop badInfw[] = { {0.0f, 4.0f}, {0.5f, INFINITY}, {1.0f, 12.0f} };
	NVGwidthStop copy[3];

	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 100, 0);

#define INVALID(ptr, n) do { \
		int calls = c.strokes; \
		CHECK(nvgStrokeProfile(ctx, (ptr), (n)) == 0); \
		CHECK(c.strokes == calls); \
	} while (0)

	INVALID(NULL, 2);
	INVALID(ok, -1);
	INVALID(one, 1);
	INVALID(badFirst, 2);
	INVALID(badLast, 2);
	INVALID(badOrder, 4);
	INVALID(badNaN, 3);
	INVALID(badInfT, 3);
	INVALID(badZero, 3);
	INVALID(badNeg, 3);
	INVALID(badNaNw, 3);
	INVALID(badInfw, 3);
#undef INVALID

	memcpy(copy, ok, sizeof(ok));
	CHECK(nvgStrokeProfile(ctx, ok, 3) == 1);
	CHECK(memcmp(copy, ok, sizeof(ok)) == 0);

	// count == 0 is an ordinary stroke, not a profile draw.
	capReset(&c);
	nvgStrokeWidth(ctx, 6.0f);
	CHECK(nvgStrokeProfile(ctx, NULL, 0) == 1);
	CHECK(c.strokes == 1);
	CHECK_NEAR(c.strokeWidth, 6.0f, 1e-5f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Curved sub-paths use the existing tessellation and support varying widths.
// --------------------------------------------------------------------------
static void testCurveProfile(void)
{
	NVGwidthStop stops[] = { {0.0f, 2.0f}, {0.5f, 12.0f}, {1.0f, 4.0f} };
	int aa;

	for (aa = 0; aa < 2; aa++) {
		Capture c;
		NVGcontext* ctx;
		capReset(&c);
		ctx = makeContext(&c, aa);
		frame(ctx, 1.0f);
		nvgLineCap(ctx, NVG_ROUND);
		nvgLineJoin(ctx, NVG_ROUND);
		nvgBeginPath(ctx);
		nvgMoveTo(ctx, 10, 10);
		nvgBezierTo(ctx, 20, 80, 80, -60, 120, 20);
		nvgLineTo(ctx, 160, 60);
		CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
		CHECK(c.strokes == 1);
		CHECK(c.npaths == 1);
		CHECK(c.pathClosed[0] == 0);
		CHECK(c.nverts > 20);
		CHECK(allFinite(&c));
		nvgEndFrame(ctx);
		nvgDeleteInternal(ctx);
	}
}

// --------------------------------------------------------------------------
// Each sub-path gets its own t domain starting at zero.
// --------------------------------------------------------------------------
static void testMultipleSubpaths(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 2.0f}, {1.0f, 10.0f} };

	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 100, 0);
	nvgMoveTo(ctx, 0, 50);
	nvgLineTo(ctx, 50, 50);
	CHECK(nvgStrokeProfile(ctx, stops, 2) == 1);
	CHECK(c.strokes == 1);
	CHECK(c.npaths == 2);
	CHECK(allFinite(&c));

	// Both starts are width 2 and both ends width 10, regardless of the
	// different sub-path lengths.
	CHECK_NEAR(minVertexDistanceNear(&c, 0, 0, 10), 1.0f, 0.1f);
	CHECK_NEAR(minVertexDistanceNear(&c, 100, 0, 10), 5.0f, 0.1f);
	CHECK_NEAR(minVertexDistanceNear(&c, 0, 50, 10), 1.0f, 0.1f);
	CHECK_NEAR(minVertexDistanceNear(&c, 50, 50, 10), 5.0f, 0.1f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// Profile widths use the same average transform scale as nvgStroke.
// --------------------------------------------------------------------------
static void testTransformScale(void)
{
	Capture c;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 4.0f}, {0.5f, 6.0f}, {1.0f, 8.0f} };
	float ymin, ymax;

	capReset(&c);
	ctx = makeContext(&c, 0);
	frame(ctx, 1.0f);
	nvgLineCap(ctx, NVG_BUTT);
	nvgScale(ctx, 2.0f, 2.0f);
	beginLine(ctx, 0, 0, 100, 0);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(c.strokes == 1);

	boundsAtX(&c, 0.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -4.0f, 1e-3f);
	CHECK_NEAR(ymax,  4.0f, 1e-3f);
	boundsAtX(&c, 100.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -6.0f, 1e-3f);
	CHECK_NEAR(ymax,  6.0f, 1e-3f);
	boundsAtX(&c, 200.0f, 1e-3f, &ymin, &ymax);
	CHECK_NEAR(ymin, -8.0f, 1e-3f);
	CHECK_NEAR(ymax,  8.0f, 1e-3f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

// --------------------------------------------------------------------------
// A profile draw leaves the shared path cache alone: a subsequent fill is
// byte-for-byte the same as it was before the profile.
// --------------------------------------------------------------------------
static void testFillUnaffected(void)
{
	Capture a, b;
	NVGcontext* ctx;
	NVGwidthStop stops[] = { {0.0f, 2.0f}, {0.5f, 9.0f}, {1.0f, 4.0f} };

	capReset(&a);
	ctx = makeContext(&a, 0);
	frame(ctx, 1.0f);
	nvgBeginPath(ctx);
	nvgMoveTo(ctx, 0, 0);
	nvgLineTo(ctx, 120, 0);
	nvgLineTo(ctx, 120, 80);
	nvgLineTo(ctx, 0, 80);
	nvgClosePath(ctx);
	nvgFillColor(ctx, nvgRGBAf(0.1f, 0.2f, 0.3f, 1.0f));
	nvgFill(ctx);
	CHECK(a.fills == 1);
	CHECK(a.fillNPaths == 1);
	CHECK(a.fillNVert > 0);

	nvgInternalParams(ctx)->userPtr = &b;
	capReset(&b);
	CHECK(nvgStrokeProfile(ctx, stops, 3) == 1);
	CHECK(b.strokes == 1);

	capReset(&b);
	nvgFill(ctx);
	CHECK(b.fills == 1);
	CHECK(b.fillNPaths == a.fillNPaths);
	CHECK(b.fillNVert == a.fillNVert);
	CHECK_NEAR((float)b.fillChecksum, (float)a.fillChecksum, 1e-4f);

	nvgEndFrame(ctx);
	nvgDeleteInternal(ctx);
}

int main(void)
{
	testStraightGradient();
	testMiterIntersection();
	testJoinsAndMiterLimit();
	testClosedSeam();
	testReverseWinding();
	testConstantAndReuse();
	testCaps();
	testAntialiasAndLocalThinWidth();
	testDevicePixelRatio();
	testCallbackState();
	testEmptyAndDegenerate();
	testValidation();
	testCurveProfile();
	testMultipleSubpaths();
	testTransformScale();
	testFillUnaffected();
	puts("STROKE_PROFILE_OK");
	return 0;
}
