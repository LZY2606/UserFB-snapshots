/*
 * Contract tests for the limited userSpaceOnUse pattern fill support.
 *
 * Build from the repository root:
 *   cc -std=c99 -O2 tests/pattern_contract.c -lm -o /tmp/y1023-pattern
 *   /tmp/y1023-pattern
 *
 * The assertions only use pixels well inside (or well outside) the painted
 * regions, so antialiasing at coverage boundaries does not affect them.
 */

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvg.h"
#include "../src/nanosvgrast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		g_failures++; \
	} \
} while (0)

static unsigned char* render(char* svg, int w, int h, float tx, float ty, float scale)
{
	NSVGimage* image = nsvgParse(svg, "px", 96);
	NSVGrasterizer* rast;
	unsigned char* buf;

	if (image == NULL)
		return NULL;
	rast = nsvgCreateRasterizer();
	if (rast == NULL) {
		nsvgDelete(image);
		return NULL;
	}
	buf = (unsigned char*)calloc((size_t)w * h, 4);
	nsvgRasterize(rast, image, tx, ty, scale, buf, w, h, w * 4);
	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);
	return buf;
}

static int pixelIs(const unsigned char* buf, int w, int x, int y,
				   unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	const unsigned char* px = &buf[(y * w + x) * 4];
	return px[0] == r && px[1] == g && px[2] == b && px[3] == a;
}

static int pixelRedOpaque(const unsigned char* buf, int w, int x, int y)
{
	return pixelIs(buf, w, x, y, 255, 0, 0, 255);
}

static int pixelTransparent(const unsigned char* buf, int w, int x, int y)
{
	// Only alpha matters: the rasterizer defringes transparent pixels,
	// borrowing RGB from opaque neighbors while leaving alpha at zero.
	return buf[(y * w + x) * 4 + 3] == 0;
}

// Base case: 4x4 tile, 2px wide red stripe, 8x4 target.
static void testBasicStripes(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;

	CHECK(pixelRedOpaque(buf, 8, 1, 1));
	CHECK(pixelRedOpaque(buf, 8, 5, 1));
	CHECK(pixelTransparent(buf, 8, 3, 1));
	CHECK(pixelTransparent(buf, 8, 7, 1));
	// Same pattern on every row, and the very first column is painted too.
	CHECK(pixelRedOpaque(buf, 8, 0, 3));
	CHECK(pixelTransparent(buf, 8, 6, 0));
	free(buf);
}

// The pattern render must match an explicit expansion of the same rectangles.
static void testMatchesExplicitExpansion(void)
{
	char patterned[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	char explicitRects[] =
		"<svg width=\"8\" height=\"4\">"
		"<rect x=\"0\" y=\"0\" width=\"2\" height=\"4\" fill=\"red\"/>"
		"<rect x=\"4\" y=\"0\" width=\"2\" height=\"4\" fill=\"red\"/></svg>";
	unsigned char* a = render(patterned, 8, 4, 0, 0, 1);
	unsigned char* b = render(explicitRects, 8, 4, 0, 0, 1);
	CHECK(a != NULL && b != NULL);
	if (a == NULL || b == NULL) { free(a); free(b); return; }

	CHECK(pixelRedOpaque(a, 8, 1, 1) == pixelRedOpaque(b, 8, 1, 1));
	CHECK(pixelRedOpaque(a, 8, 5, 1) == pixelRedOpaque(b, 8, 5, 1));
	CHECK(pixelTransparent(a, 8, 3, 1) == pixelTransparent(b, 8, 3, 1));
	CHECK(pixelTransparent(a, 8, 7, 1) == pixelTransparent(b, 8, 7, 1));
	free(a);
	free(b);
}

// Negative tile origin: stripes cover [-1,1), [3,5), [7,9).
static void testNegativeOrigin(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"-1\" width=\"4\" height=\"4\">"
		"<rect x=\"-1\" width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;

	CHECK(pixelRedOpaque(buf, 8, 0, 1));	// [0,1)
	CHECK(pixelTransparent(buf, 8, 2, 1));	// gap [1,3)
	CHECK(pixelRedOpaque(buf, 8, 4, 1));	// [3,5)
	CHECK(pixelTransparent(buf, 8, 6, 1));	// gap [5,7)
	CHECK(pixelRedOpaque(buf, 8, 7, 1));	// [7,8) part of [7,9)
	free(buf);
}

// Content overflowing the tile is clipped: rect x=1 width=6 in a tile starting
// at -1 is clipped to [1,3); the gap [-1,1) must stay empty.
static void testTileClipping(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"-1\" width=\"4\" height=\"4\">"
		"<rect x=\"1\" width=\"6\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;

	CHECK(pixelTransparent(buf, 8, 0, 1));	// gap [-1,1)
	CHECK(pixelRedOpaque(buf, 8, 2, 1));	// clipped stripe [1,3)
	CHECK(pixelTransparent(buf, 8, 4, 1));	// gap [3,5): overflow must not fill it
	CHECK(pixelRedOpaque(buf, 8, 6, 1));	// [5,7)
	CHECK(pixelTransparent(buf, 8, 7, 1));	// gap [7,9)
	free(buf);
}

// patternTransform rotate(90) combined with a non-uniform shape scale. The
// two matrices do not commute, so the assertions pin down their order.
static void testRotateAndShapeScale(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\""
		" patternTransform=\"rotate(90)\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<g transform=\"scale(2,1)\">"
		"<rect width=\"4\" height=\"8\" fill=\"url(#p)\"/>"
		"</g></svg>";
	unsigned char* buf = render(svg, 8, 8, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;

	// Horizontal stripes y in [0,2), [4,6), spanning the whole width.
	CHECK(pixelRedOpaque(buf, 8, 1, 1));
	CHECK(pixelRedOpaque(buf, 8, 6, 1));
	CHECK(pixelTransparent(buf, 8, 1, 3));	// would be red if the transform order were reversed
	CHECK(pixelRedOpaque(buf, 8, 6, 5));
	CHECK(pixelTransparent(buf, 8, 1, 7));
	free(buf);
}

// patternTransform skewX(45); also exercises negative modulo in pattern space.
static void testSkew(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\""
		" patternTransform=\"skewX(45)\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"8\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 8, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;

	// Red iff (x - y) mod 4 lies in [0,2), evaluated on full pixels.
	CHECK(pixelRedOpaque(buf, 8, 5, 1));	// x-y in [4,6)
	CHECK(pixelTransparent(buf, 8, 3, 1));	// x-y in [2,4)
	CHECK(pixelRedOpaque(buf, 8, 7, 3));	// x-y in [4,6)
	CHECK(pixelRedOpaque(buf, 8, 2, 5));	// x-y = -3 -> floor-mod 1, inside a negative tile
	CHECK(pixelTransparent(buf, 8, 1, 6));	// x-y = -5 -> floor-mod 3, a negative tile gap
	CHECK(pixelTransparent(buf, 8, 7, 1));	// x-y in [6,8)
	free(buf);
}

// One pattern reused by two shapes; swapping document order must not change
// either shape's tiling.
static void testReuseAndOrder(void)
{
	char orderAB[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"4\" y=\"4\" width=\"4\" height=\"4\" fill=\"url(#p)\"/></svg>";
	char orderBA[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect x=\"4\" y=\"4\" width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* ab = render(orderAB, 8, 8, 0, 0, 1);
	unsigned char* ba = render(orderBA, 8, 8, 0, 0, 1);
	CHECK(ab != NULL && ba != NULL);
	if (ab == NULL || ba == NULL) { free(ab); free(ba); return; }

	CHECK(memcmp(ab, ba, (size_t)8 * 8 * 4) == 0);
	// Both shapes show the same user-space stripe phase.
	CHECK(pixelRedOpaque(ab, 8, 1, 1));		// first shape
	CHECK(pixelTransparent(ab, 8, 3, 1));
	CHECK(pixelRedOpaque(ab, 8, 5, 5));		// second shape, x=5 mod 4 = 1
	CHECK(pixelTransparent(ab, 8, 7, 5));
	free(ab);
	free(ba);
}

// Unsupported patterns degrade to no fill; ordinary shapes still render.
static void testInvalidPatterns(void)
{
	char objectBBox[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"objectBoundingBox\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char badSize[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"0\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char strokedChild[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\" stroke=\"blue\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char unknownRef[] =
		"<svg width=\"4\" height=\"4\">"
		"<rect width=\"4\" height=\"4\" fill=\"url(#nope)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char contentUnits[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\""
		" patternContentUnits=\"objectBoundingBox\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char hrefInherit[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\""
		" xlink:href=\"#q\"><rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char singularTransform[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\""
		" patternTransform=\"matrix(0 0 0 0 0 0)\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char nonRectContent[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<circle cx=\"2\" cy=\"2\" r=\"2\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	char* cases[8] = { objectBBox, badSize, strokedChild, unknownRef,
	                   contentUnits, hrefInherit, singularTransform, nonRectContent };
	int i;

	for (i = 0; i < 8; i++) {
		unsigned char* buf = render(cases[i], 4, 4, 0, 0, 1);
		CHECK(buf != NULL);
		if (buf == NULL) continue;
		// Pattern fill resolves to none, so the top rect contributes nothing.
		CHECK(pixelTransparent(buf, 4, 3, 3));
		// The following plain shape renders normally.
		CHECK(pixelIs(buf, 4, 1, 1, 0, 128, 0, 255));
		free(buf);
	}
}

// One parse result rasterized repeatedly with different tx/ty/scale must not
// reuse any device-space result from a previous run.
static void testReRasterizeParameters(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	NSVGimage* image = nsvgParse(svg, "px", 96);
	NSVGrasterizer* rast;
	unsigned char* big;
	unsigned char* shifted;

	CHECK(image != NULL);
	if (image == NULL) return;
	rast = nsvgCreateRasterizer();
	CHECK(rast != NULL);
	if (rast == NULL) { nsvgDelete(image); return; }

	big = (unsigned char*)calloc(16 * 8, 4);
	nsvgRasterize(rast, image, 0, 0, 2, big, 16, 8, 16 * 4);
	// Scale 2: red x in [0,4), [8,12).
	CHECK(pixelRedOpaque(big, 16, 2, 2));
	CHECK(pixelTransparent(big, 16, 6, 2));
	CHECK(pixelRedOpaque(big, 16, 10, 2));
	CHECK(pixelTransparent(big, 16, 14, 2));

	shifted = (unsigned char*)calloc(8 * 4, 4);
	nsvgRasterize(rast, image, 1, 0, 1, shifted, 8, 4, 8 * 4);
	// Translation 1: red device x in [1,3), [5,7).
	CHECK(pixelRedOpaque(shifted, 8, 1, 1));
	CHECK(pixelTransparent(shifted, 8, 4, 1));
	CHECK(pixelRedOpaque(shifted, 8, 5, 1));
	CHECK(pixelTransparent(shifted, 8, 0, 1));

	free(big);
	free(shifted);
	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);
}

// Rectangles declared inside <defs> must never be painted as main shapes.
static void testDefsContentNotPainted(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	// If the pattern child rect were painted directly, x=5 would stay empty.
	CHECK(pixelRedOpaque(buf, 8, 5, 1));
	free(buf);
}

// Pattern paint on a stroke is out of scope: it degrades to no stroke and
// must not disturb later shapes.
static void testPatternOnStrokeUnsupported(void)
{
	char svg[] =
		"<svg width=\"4\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"4\" height=\"4\" fill=\"none\" stroke=\"url(#p)\" stroke-width=\"2\"/>"
		"<rect x=\"0\" y=\"0\" width=\"4\" height=\"2\" fill=\"green\"/></svg>";
	unsigned char* buf = render(svg, 4, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	CHECK(pixelTransparent(buf, 4, 0, 3));	// no pattern stroke painted in the corner band
	CHECK(pixelIs(buf, 4, 1, 1, 0, 128, 0, 255));
	free(buf);
}

// Content rects are painted in document order: a later rect may cover an
// earlier one, both inside one tile and in the explicit-expansion reference.
static void testCoveringRects(void)
{
	char patterned[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"4\" height=\"4\" fill=\"red\"/>"
		"<rect x=\"1\" width=\"2\" height=\"4\" fill=\"green\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	char explicitRects[] =
		"<svg width=\"8\" height=\"4\">"
		"<rect x=\"0\" width=\"4\" height=\"4\" fill=\"red\"/>"
		"<rect x=\"4\" width=\"4\" height=\"4\" fill=\"red\"/>"
		"<rect x=\"1\" width=\"2\" height=\"4\" fill=\"green\"/>"
		"<rect x=\"5\" width=\"2\" height=\"4\" fill=\"green\"/></svg>";
	unsigned char* a = render(patterned, 8, 4, 0, 0, 1);
	unsigned char* b = render(explicitRects, 8, 4, 0, 0, 1);
	CHECK(a != NULL && b != NULL);
	if (a == NULL || b == NULL) { free(a); free(b); return; }

	CHECK(pixelIs(a, 8, 0, 1, 255, 0, 0, 255));		// red survives on the edges
	CHECK(pixelIs(a, 8, 1, 1, 0, 128, 0, 255));		// green covers red
	CHECK(pixelIs(a, 8, 2, 1, 0, 128, 0, 255));
	CHECK(pixelIs(a, 8, 3, 1, 255, 0, 0, 255));
	CHECK(pixelIs(a, 8, 5, 1, 0, 128, 0, 255));
	CHECK(pixelIs(a, 8, 7, 1, 255, 0, 0, 255));
	CHECK(memcmp(a, b, (size_t)8 * 4 * 4) == 0);
	free(a);
	free(b);
}

// A viewBox scales user coordinates: the tile must scale with the geometry
// exactly once (not be double-transformed).
static void testViewBoxScaling(void)
{
	char svg[] =
		"<svg width=\"16\" height=\"8\" viewBox=\"0 0 8 4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 16, 8, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	// Everything is doubled: red device x in [0,4) and [8,12).
	CHECK(pixelRedOpaque(buf, 16, 2, 2));
	CHECK(pixelRedOpaque(buf, 16, 10, 2));
	CHECK(pixelTransparent(buf, 16, 6, 2));
	CHECK(pixelTransparent(buf, 16, 14, 2));
	free(buf);
}

// The referencing shape's own group transform moves the tiling with it.
static void testShapeGroupTransform(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<g transform=\"translate(2,2)\">"
		"<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
		"</g></svg>";
	unsigned char* buf = render(svg, 8, 8, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	// Shape covers [2,6); stripe phase is translated along with it.
	CHECK(pixelTransparent(buf, 8, 1, 3));
	CHECK(pixelRedOpaque(buf, 8, 3, 3));
	CHECK(pixelTransparent(buf, 8, 5, 3));
	CHECK(pixelTransparent(buf, 8, 3, 1));	// above the shape
	free(buf);
}

// patternTransform translation shifts the tile origin in pattern space.
static void testPatternTransformTranslate(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\""
		" patternTransform=\"translate(2,0)\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/>"
		"</pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	// Red device x in [2,4), [6,8).
	CHECK(pixelTransparent(buf, 8, 0, 1));
	CHECK(pixelRedOpaque(buf, 8, 2, 1));
	CHECK(pixelTransparent(buf, 8, 4, 1));
	CHECK(pixelRedOpaque(buf, 8, 6, 1));
	free(buf);
}

// Shape opacity applies to the pattern paint.
static void testShapeOpacity(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"4\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<rect width=\"8\" height=\"4\" fill=\"url(#p)\" opacity=\"0.5\"/></svg>";
	unsigned char* buf = render(svg, 8, 4, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	// Interior coverage is full, so alpha comes from opacity alone.
	CHECK(buf[(1*8+1)*4+3] == 127);
	CHECK(buf[(1*8+1)*4+0] == 255);
	free(buf);
}

// The referencing shape keeps using the normal path pipeline, including
// fill-rule: an even-odd ring leaves its hole transparent.
static void testEvenOddPath(void)
{
	char svg[] =
		"<svg width=\"8\" height=\"8\">"
		"<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
		"<rect width=\"2\" height=\"4\" fill=\"red\"/></pattern></defs>"
		"<path fill-rule=\"evenodd\" fill=\"url(#p)\""
		" d=\"M0 0H8V8H0Z M2 2H6V6H2Z\"/></svg>";
	unsigned char* buf = render(svg, 8, 8, 0, 0, 1);
	CHECK(buf != NULL);
	if (buf == NULL) return;
	CHECK(pixelRedOpaque(buf, 8, 1, 1));	// ring
	CHECK(pixelTransparent(buf, 8, 3, 3));	// hole
	CHECK(pixelTransparent(buf, 8, 4, 4));
	free(buf);
}

int main(void)
{
	testBasicStripes();
	testMatchesExplicitExpansion();
	testNegativeOrigin();
	testTileClipping();
	testCoveringRects();
	testViewBoxScaling();
	testShapeGroupTransform();
	testPatternTransformTranslate();
	testRotateAndShapeScale();
	testSkew();
	testReuseAndOrder();
	testInvalidPatterns();
	testPatternOnStrokeUnsupported();
	testReRasterizeParameters();
	testDefsContentNotPainted();
	testShapeOpacity();
	testEvenOddPath();

	if (g_failures == 0)
		printf("pattern_contract: all tests passed\n");
	else
		printf("pattern_contract: %d check(s) failed\n", g_failures);
	return g_failures != 0;
}
