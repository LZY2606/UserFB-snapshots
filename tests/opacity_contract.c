/*
 * Opacity contract tests for NanoSVG.
 *
 * These tests enter through the public parse and rasterize entry points only.
 * Reference pixels are computed independently as premultiplied source-over
 * composites, then compared with the non-premultiplied RGBA output.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define NANOSVG_IMPLEMENTATION
#include "../src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvgrast.h"

typedef struct PM {
	double r, g, b, a;	/* premultiplied, 0..1 */
} PM;

static int failures = 0;

static PM pm_solid(int r, int g, int b, int a)
{
	PM p;
	double alpha = (double)a / 255.0;
	p.r = (double)r / 255.0 * alpha;
	p.g = (double)g / 255.0 * alpha;
	p.b = (double)b / 255.0 * alpha;
	p.a = alpha;
	return p;
}

static PM pm_fade(PM c, double opacity)
{
	c.r *= opacity;
	c.g *= opacity;
	c.b *= opacity;
	c.a *= opacity;
	return c;
}

/* Source-over: s on top of d, both premultiplied. */
static PM pm_over(PM d, PM s)
{
	PM o;
	double inv = 1.0 - s.a;
	o.r = s.r + d.r * inv;
	o.g = s.g + d.g * inv;
	o.b = s.b + d.b * inv;
	o.a = s.a + d.a * inv;
	return o;
}

static int near_byte(double v)
{
	if (v < 0.0) v = 0.0;
	if (v > 255.0) v = 255.0;
	return (int)(v + 0.5);
}

static void report(const char* label, int x, int y,
				   int got, int want)
{
	printf("FAIL %s pixel(%d,%d): got %d expected %d\n", label, x, y, got, want);
}

/* Compares one pixel of the unpremultiplied output with a premultiplied reference. */
static int check_pixel(const char* label, const unsigned char* pixels,
					   int width, int x, int y, PM want, int tol)
{
	const unsigned char* p = &pixels[(y * width + x) * 4];
	int ea = near_byte(want.a * 255.0);
	int er = want.a > 0.0 ? near_byte(want.r / want.a * 255.0) : 0;
	int eg = want.a > 0.0 ? near_byte(want.g / want.a * 255.0) : 0;
	int eb = want.a > 0.0 ? near_byte(want.b / want.a * 255.0) : 0;
	int ok = 1;
	if (abs((int)p[0] - er) > tol) { report(label, x, y, p[0], er); ok = 0; }
	if (abs((int)p[1] - eg) > tol) { report(label, x, y, p[1], eg); ok = 0; }
	if (abs((int)p[2] - eb) > tol) { report(label, x, y, p[2], eb); ok = 0; }
	if (abs((int)p[3] - ea) > tol) { report(label, x, y, p[3], ea); ok = 0; }
	return ok;
}

static NSVGimage* parse_svg(const char* xml)
{
	char* text = (char*)malloc(strlen(xml) + 1);
	NSVGimage* image;
	if (text == NULL) return NULL;
	strcpy(text, xml);
	image = nsvgParse(text, "px", 96);
	free(text);
	return image;
}

static unsigned char* make_buffer(int w, int h, int stride, int fill)
{
	size_t size = (size_t)stride * (size_t)h;
	unsigned char* p = (unsigned char*)malloc(size);
	(void)w;
	if (p != NULL)
		memset(p, fill, size);
	return p;
}

static void draw(NSVGrasterizer* r, NSVGimage* image,
				 float tx, float ty, float scale,
				 unsigned char* dst, int w, int h, int stride)
{
	nsvgRasterize(r, image, tx, ty, scale, dst, w, h, stride);
}

static int buffers_equal(const unsigned char* a, const unsigned char* b, int w, int h, int stride)
{
	int y;
	for (y = 0; y < h; y++) {
		if (memcmp(&a[y * stride], &b[y * stride], (size_t)w * 4) != 0)
			return 0;
	}
	return 1;
}

static void section_result(const char* marker, int base_failures)
{
	if (failures == base_failures)
		printf("%s\n", marker);
	else
		printf("SECTION FAILED: %s\n", marker);
}

/*
 * Group section: siblings composed inside a group, background behind the
 * group, and a later sibling that must not be swallowed by the group.
 */
static void test_group(void)
{
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* out = make_buffer(24, 16, 24 * 4, 0);
	NSVGimage* im;
	int base = failures;
	PM red = pm_solid(255, 0, 0, 255);
	PM blue = pm_solid(0, 0, 255, 255);
	int ok;

	ok = (r != NULL) && (out != NULL);
	if (!ok) { printf("FAIL allocation\n"); failures++; }

	/* Overlapping opaque siblings inside one half-opacity group. */
	im = parse_svg("<svg width='24' height='16'><g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/>"
				   "</g></svg>");
	if (im == NULL) { printf("FAIL parse group\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		/* Left region: red opaque, faded once by the group. */
		failures += !check_pixel("group-left", out, 24, 4, 6, pm_fade(red, .5), 2);
		/* Overlap: blue wins source-over inside the group, then fades once.
		   It must not be alpha ~190 or carry red. */
		failures += !check_pixel("group-overlap", out, 24, 10, 6, pm_fade(blue, .5), 2);
		failures += !check_pixel("group-right", out, 24, 18, 6, pm_fade(blue, .5), 2);
		failures += !check_pixel("group-empty", out, 24, 0, 0, (PM){0,0,0,0}, 2);
		nsvgDelete(im);
	}

	/* Background behind the group and an opaque sibling painted afterwards. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='0' y='0' width='24' height='16' fill='white'/>"
				   "<g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/>"
				   "</g>"
				   "<rect x='20' y='12' width='4' height='4' fill='#00ff00'/>"
				   "</svg>");
	if (im == NULL) { printf("FAIL parse background\n"); failures++; }
	else {
		PM white = pm_solid(255, 255, 255, 255);
		PM green = pm_solid(0, 255, 0, 255);
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("group-bg-left", out, 24, 4, 6,
								 pm_over(white, pm_fade(red, .5)), 2);
		failures += !check_pixel("group-bg-overlap", out, 24, 10, 6,
								 pm_over(white, pm_fade(blue, .5)), 2);
		failures += !check_pixel("group-later-sibling", out, 24, 21, 13, green, 2);
		nsvgDelete(im);
	}

	/* A semi-transparent child over a background inside the group. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='0' y='0' width='24' height='16' fill='white'/>"
				   "<g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red' fill-opacity='.5'/>"
				   "</g></svg>");
	if (im == NULL) { printf("FAIL parse semi child\n"); failures++; }
	else {
		PM white = pm_solid(255, 255, 255, 255);
		PM semi_red = pm_solid(255, 0, 0, 128);
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("group-semi-child-bg", out, 24, 4, 6,
								 pm_over(white, pm_fade(semi_red, .5)), 2);
		nsvgDelete(im);
	}

	/* Opaque control: proves the rasterizer really paints, not just blanks. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='1' y='1' width='14' height='12' fill='blue'/></svg>");
	if (im == NULL) { printf("FAIL parse opaque control\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("opaque-control", out, 24, 4, 6, blue, 1);
		nsvgDelete(im);
	}

	section_result("OPACITY_GROUP_OK", base);
	free(out);
	nsvgDeleteRasterizer(r);
}

/* Nesting section: group opacity applied once per level, plus transforms. */
static void test_nesting(void)
{
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* out = make_buffer(24, 16, 24 * 4, 0);
	NSVGimage* im;
	int base = failures;
	PM red = pm_solid(255, 0, 0, 255);
	PM white = pm_solid(255, 255, 255, 255);

	im = parse_svg("<svg width='24' height='16'>"
				   "<g opacity='.5'><g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "</g></g></svg>");
	if (im == NULL) { printf("FAIL parse nested two\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		/* 255 * .5 * .5 = 63.75 */
		failures += !check_pixel("nested-two", out, 24, 4, 6,
								 pm_fade(pm_fade(red, .5), .5), 2);
		nsvgDelete(im);
	}

	im = parse_svg("<svg width='24' height='16'>"
				   "<g opacity='.5'><g opacity='.5'><g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "</g></g></g></svg>");
	if (im == NULL) { printf("FAIL parse nested three\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		/* 255 * .5^3 = 31.875 */
		failures += !check_pixel("nested-three", out, 24, 4, 6,
								 pm_fade(pm_fade(pm_fade(red, .5), .5), .5), 2);
		nsvgDelete(im);
	}

	/* Background outside a nested pair: group results compose at their place. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='0' y='0' width='24' height='16' fill='white'/>"
				   "<g opacity='.5'><g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "</g></g></svg>");
	if (im == NULL) { printf("FAIL parse nested background\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("nested-two-bg", out, 24, 4, 6,
								 pm_over(white, pm_fade(pm_fade(red, .5), .5)), 2);
		nsvgDelete(im);
	}

	/* Nested transforms: content must move as a whole and leave no residue. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<g opacity='.5' transform='translate(8,0)'>"
				   "<g opacity='.5' transform='translate(0,2)'>"
				   "<rect x='1' y='1' width='4' height='4' fill='red'/>"
				   "</g></g></svg>");
	if (im == NULL) { printf("FAIL parse nested transform\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("nested-transform-new", out, 24, 9, 5,
								 pm_fade(pm_fade(red, .5), .5), 2);
		failures += !check_pixel("nested-transform-old", out, 24, 3, 3,
								 (PM){0,0,0,0}, 2);
		nsvgDelete(im);
	}

	section_result("OPACITY_NESTING_OK", base);
	free(out);
	nsvgDeleteRasterizer(r);
}

/* Element section: shape opacity vs fill/stroke, style/class, empty/invisible. */
static void test_element(void)
{
	NSVGrasterizer* r = nsvgCreateRasterizer();
	unsigned char* out = make_buffer(24, 16, 24 * 4, 0);
	unsigned char* style_out = make_buffer(24, 16, 24 * 4, 0);
	unsigned char* class_out = make_buffer(24, 16, 24 * 4, 0);
	NSVGimage* im;
	int base = failures;
	PM blue = pm_solid(0, 0, 255, 255);
	PM red = pm_solid(255, 0, 0, 255);
	PM white = pm_solid(255, 255, 255, 255);

	/* Fill and stroke composed first; element opacity applied once after.
	   Rect y=4..12, stroke width 6 => top stroke band y=1..7 over fill.
	   fill-only at y=8; stroke-only at y=2; overlap at y=5. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='4' y='4' width='16' height='8' fill='red' stroke='blue'"
				   " stroke-width='6' opacity='.5'/></svg>");
	if (im == NULL) { printf("FAIL parse fill/stroke\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("shape-fill-only", out, 24, 10, 8, pm_fade(red, .5), 2);
		failures += !check_pixel("shape-stroke-only", out, 24, 10, 2, pm_fade(blue, .5), 2);
		/* Default paint order: fill then stroke, so overlap is blue, alpha once. */
		failures += !check_pixel("shape-overlap-default", out, 24, 10, 5, pm_fade(blue, .5), 2);
		nsvgDelete(im);
	}

	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='4' y='4' width='16' height='8' fill='red' stroke='blue'"
				   " stroke-width='6' opacity='.5' paint-order='stroke fill markers'/></svg>");
	if (im == NULL) { printf("FAIL parse reverse paint order\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("shape-overlap-reverse", out, 24, 10, 5, pm_fade(red, .5), 2);
		failures += !check_pixel("shape-rev-stroke-only", out, 24, 10, 2, pm_fade(blue, .5), 2);
		nsvgDelete(im);
	}

	/* The same group opacity through attribute, style, and class entry points. */
	im = parse_svg("<svg width='24' height='16'><g opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/>"
				   "</g></svg>");
	if (im == NULL) { printf("FAIL parse attr entry\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		nsvgDelete(im);
	}
	im = parse_svg("<svg width='24' height='16'><g style='opacity:.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/>"
				   "</g></svg>");
	if (im == NULL) { printf("FAIL parse style entry\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, style_out, 24, 16, 24 * 4);
		nsvgDelete(im);
	}
	im = parse_svg("<svg width='24' height='16'><style>.half{opacity:.5}</style>"
				   "<g class='half'><rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/></g></svg>");
	if (im == NULL) { printf("FAIL parse class entry\n"); failures++; }
	else {
		draw(r, im, 0, 0, 1, class_out, 24, 16, 24 * 4);
		nsvgDelete(im);
	}
	failures += !check_pixel("attr-entry", out, 24, 10, 6, pm_fade(blue, .5), 2);
	if (!buffers_equal(out, style_out, 24, 16, 24 * 4)) {
		printf("FAIL style entry differs from attribute entry\n");
		failures++;
	}
	if (!buffers_equal(out, class_out, 24, 16, 24 * 4)) {
		printf("FAIL class entry differs from attribute entry\n");
		failures++;
	}

	/* Empty and invisible groups leave the background untouched. */
	im = parse_svg("<svg width='24' height='16'>"
				   "<rect x='0' y='0' width='24' height='16' fill='white'/>"
				   "<g opacity='.5'></g>"
				   "<g opacity='.5'><rect x='1' y='1' width='4' height='4' fill='red' display='none'/></g>"
				   "<g opacity='0'><rect x='8' y='1' width='4' height='4' fill='blue'/></g>"
				   "<g opacity='1'><rect x='16' y='1' width='4' height='4' fill='#00ff00'/></g>"
				   "</svg>");
	if (im == NULL) { printf("FAIL parse invisible groups\n"); failures++; }
	else {
		PM green = pm_solid(0, 255, 0, 255);
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("empty-group-bg", out, 24, 3, 3, white, 2);
		failures += !check_pixel("display-none-group-bg", out, 24, 3, 3, white, 2);
		failures += !check_pixel("opacity-zero-group-bg", out, 24, 9, 3, white, 2);
		failures += !check_pixel("opacity-one-group", out, 24, 18, 3, green, 2);
		nsvgDelete(im);
	}

	/* fill-opacity keeps its inherited per-paint meaning, unlike group opacity:
	   overlap becomes a blend with alpha ~191 rather than one faded blue. */
	im = parse_svg("<svg width='24' height='16'><g fill-opacity='.5'>"
				   "<rect x='1' y='1' width='14' height='12' fill='red'/>"
				   "<rect x='8' y='1' width='14' height='12' fill='blue'/>"
				   "</g></svg>");
	if (im == NULL) { printf("FAIL parse fill-opacity inheritance\n"); failures++; }
	else {
		PM semi_red = pm_solid(255, 0, 0, 128);
		PM semi_blue = pm_solid(0, 0, 255, 128);
		draw(r, im, 0, 0, 1, out, 24, 16, 24 * 4);
		failures += !check_pixel("fill-opacity-left", out, 24, 4, 6, semi_red, 2);
		failures += !check_pixel("fill-opacity-overlap", out, 24, 10, 6,
								 pm_over(semi_red, semi_blue), 2);
		nsvgDelete(im);
	}

	section_result("OPACITY_ELEMENT_OK", base);
	free(out);
	free(style_out);
	free(class_out);
	nsvgDeleteRasterizer(r);
}

/* Reuse section: repeated renders, viewport changes, rasterizer reuse, stride. */
static void test_reuse(void)
{
	NSVGrasterizer* r = nsvgCreateRasterizer();
	const char* group_svg =
		"<svg width='24' height='16'><g opacity='.5'>"
		"<rect x='1' y='1' width='14' height='12' fill='red'/>"
		"<rect x='8' y='1' width='14' height='12' fill='blue'/></g></svg>";
	const char* other_svg =
		"<svg width='24' height='16'><rect x='2' y='2' width='8' height='8' fill='#00ff00'/></svg>";
	unsigned char* first = make_buffer(24, 16, 24 * 4, 0);
	unsigned char* again = make_buffer(24, 16, 24 * 4, 0);
	unsigned char* shifted = make_buffer(24, 16, 24 * 4, 0);
	unsigned char* scaled = make_buffer(48, 32, 48 * 4, 0);
	unsigned char* strided = make_buffer(24, 16, 24 * 4 + 16, 77);
	NSVGimage* im = parse_svg(group_svg);
	NSVGimage* other;
	int base = failures;
	int y, x;
	PM red = pm_solid(255, 0, 0, 255);
	PM blue = pm_solid(0, 0, 255, 255);

	if (im == NULL) { printf("FAIL parse reuse image\n"); failures++; }

	/* The same image and rasterizer rendered twice must agree exactly. */
	draw(r, im, 0, 0, 1, first, 24, 16, 24 * 4);
	draw(r, im, 0, 0, 1, again, 24, 16, 24 * 4);
	if (!buffers_equal(first, again, 24, 16, 24 * 4)) {
		printf("FAIL repeated render differs\n");
		failures++;
	}
	failures += !check_pixel("reuse-left", first, 24, 4, 6, pm_fade(red, .5), 2);
	failures += !check_pixel("reuse-overlap", first, 24, 10, 6, pm_fade(blue, .5), 2);

	/* Different device parameters on the same image. */
	draw(r, im, 3, 2, 1, shifted, 24, 16, 24 * 4);
	failures += !check_pixel("reuse-shifted-left", shifted, 24, 7, 8, pm_fade(red, .5), 2);
	failures += !check_pixel("reuse-shifted-overlap", shifted, 24, 13, 8, pm_fade(blue, .5), 2);

	draw(r, im, 0, 0, 2, scaled, 48, 32, 48 * 4);
	/* Device coordinate is image coordinate times scale. */
	failures += !check_pixel("reuse-scaled-left", scaled, 48, 8, 12, pm_fade(red, .5), 2);
	failures += !check_pixel("reuse-scaled-overlap", scaled, 48, 20, 12, pm_fade(blue, .5), 2);

	/* Restore the original parameters: no device-space result may stick around. */
	draw(r, im, 0, 0, 1, again, 24, 16, 24 * 4);
	if (!buffers_equal(first, again, 24, 16, 24 * 4)) {
		printf("FAIL device-space composition leaked across renders\n");
		failures++;
	}

	/* Alternate images on the same rasterizer, then come back to the first. */
	other = parse_svg(other_svg);
	if (other == NULL) { printf("FAIL parse other image\n"); failures++; }
	else {
		unsigned char* other_out = make_buffer(24, 16, 24 * 4, 0);
		draw(r, other, 0, 0, 1, other_out, 24, 16, 24 * 4);
		failures += !check_pixel("reuse-other-image", other_out, 24, 6, 6,
								 pm_solid(0, 255, 0, 255), 2);
		draw(r, im, 0, 0, 1, again, 24, 16, 24 * 4);
		if (!buffers_equal(first, again, 24, 16, 24 * 4)) {
			printf("FAIL rasterizer reuse across images\n");
			failures++;
		}
		free(other_out);
		nsvgDelete(other);
	}

	/* A larger stride must leave the row padding untouched. */
	draw(r, im, 0, 0, 1, strided, 24, 16, 24 * 4 + 16);
	failures += !check_pixel("reuse-strided-overlap", strided, 24, 10, 6, pm_fade(blue, .5), 2);
	for (y = 0; y < 16; y++) {
		for (x = 24 * 4; x < 24 * 4 + 16; x++) {
			if (strided[y * (24 * 4 + 16) + x] != 77) {
				printf("FAIL stride padding modified at row %d byte %d\n", y, x);
				failures++;
				break;
			}
		}
	}

	section_result("OPACITY_REUSE_OK", base);
	free(first);
	free(again);
	free(shifted);
	free(scaled);
	free(strided);
	nsvgDelete(im);
	nsvgDeleteRasterizer(r);
}

int main(void)
{
	test_group();
	test_nesting();
	test_element();
	test_reuse();
	if (failures != 0) {
		printf("OPACITY_CONTRACT_FAILED (%d checks)\n", failures);
		return 1;
	}
	printf("OPACITY_CONTRACT_OK\n");
	return 0;
}
