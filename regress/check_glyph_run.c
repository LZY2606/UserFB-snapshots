// Regression test for nvgGlyphRun / nvgGlyphRunBounds.
// Uses the recording render backend (no GL) and stb_truetype font outline
// metrics as ground truth. Run via regress/check_glyph_run.sh from repo root.
#include "nanovg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define MAX_VERTS 8192

static NVGvertex g_verts[MAX_VERTS];
static int g_nverts;
static int g_drawcalls;
static int g_updates;
static float g_alpha;
static float g_scissorExtent0;

static int r_create(void* p) { (void)p; return 1; }
static int r_tex(void* p, int t, int w, int h, int f, const unsigned char* d) { (void)p; (void)t; (void)w; (void)h; (void)f; (void)d; return 1; }
static int r_del(void* p, int i) { (void)p; (void)i; return 1; }
static int r_size(void* p, int i, int* w, int* h) { (void)p; (void)i; *w = *h = 512; return 1; }
static int r_update(void* p, int i, int x, int y, int w, int h, const unsigned char* d) { (void)p; (void)i; (void)x; (void)y; assert(w > 0 && h > 0 && d); g_updates++; return 1; }
static void r_viewport(void* p, float w, float h, float r) { (void)p; (void)w; (void)h; (void)r; }
static void r_flush(void* p) { (void)p; }
static void r_draw(void* p, NVGpaint* paint, NVGcompositeOperationState op, NVGscissor* sc, const NVGvertex* v, int n, float fringe)
{
	int i;
	(void)p; (void)op; (void)fringe;
	g_alpha = paint->innerColor.a;
	g_scissorExtent0 = sc->extent[0];
	for (i = 0; i < n; i++) {
		assert(g_nverts < MAX_VERTS);
		assert(isfinite(v[i].x) && isfinite(v[i].y));
		assert(v[i].u >= 0.0f && v[i].u <= 1.0f);
		assert(v[i].v >= 0.0f && v[i].v <= 1.0f);
		g_verts[g_nverts++] = v[i];
	}
	g_drawcalls++;
}

static void rec_reset(void)
{
	g_nverts = 0;
	g_drawcalls = 0;
	g_updates = 0;
	g_alpha = 0.0f;
	g_scissorExtent0 = 0.0f;
}

static NVGcontext* make_ctx(void)
{
	NVGparams p;
	memset(&p, 0, sizeof(p));
	p.renderCreate = r_create;
	p.renderCreateTexture = r_tex;
	p.renderDeleteTexture = r_del;
	p.renderUpdateTexture = r_update;
	p.renderGetTextureSize = r_size;
	p.renderViewport = r_viewport;
	p.renderFlush = r_flush;
	p.renderTriangles = r_draw;
	return nvgCreateInternal(&p);
}

static stbtt_fontinfo g_stb;      /* Roboto-Regular */
static stbtt_fontinfo g_stbBold;  /* Roboto-Bold */

static void load_stb(stbtt_fontinfo* info, const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	unsigned char* d;
	assert(f != NULL);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = (unsigned char*)malloc((size_t)n);
	assert(d != NULL);
	assert(fread(d, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	assert(stbtt_InitFont(info, d, stbtt_GetFontOffsetForIndex(d, 0)));
}

static int feq(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

/* min/max of the first 6 recorded verts (one quad) */
static void quad_bounds(const NVGvertex* v, float* x0, float* y0, float* x1, float* y1)
{
	int i;
	*x0 = *x1 = v[0].x;
	*y0 = *y1 = v[0].y;
	for (i = 1; i < 6; i++) {
		if (v[i].x < *x0) *x0 = v[i].x;
		if (v[i].x > *x1) *x1 = v[i].x;
		if (v[i].y < *y0) *y0 = v[i].y;
		if (v[i].y > *y1) *y1 = v[i].y;
	}
}

/* Expected local quad of a glyph drawn at baseline origin (ox,oy). The bitmap
   is rasterized at fontSize*scale pixels (scale = fontScale * devicePxRatio)
   and the quad is mapped back to local units by dividing by scale.
   Derived from the font outline metrics (2px pad, 1px inset). */
static void expected_quad(int glyph, float fontSize, float scale, float ox, float oy, float* x0, float* y0, float* x1, float* y1)
{
	float px = fontSize * scale;
	float s = stbtt_ScaleForMappingEmToPixels(&g_stb, px);
	int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
	stbtt_GetGlyphBitmapBox(&g_stb, glyph, s, s, &bx0, &by0, &bx1, &by1);
	*x0 = ox + (float)(bx0 - 1) / scale;
	*y0 = oy + (float)(by0 - 1) / scale;
	*x1 = ox + (float)(bx1 + 1) / scale;
	*y1 = oy + (float)(by1 + 1) / scale;
}

static void assert_quad_matches(const NVGvertex* v, float x0, float y0, float x1, float y1, float eps)
{
	float ax0, ay0, ax1, ay1;
	quad_bounds(v, &ax0, &ay0, &ax1, &ay1);
	assert(feq(ax0, x0, eps) && feq(ay0, y0, eps));
	assert(feq(ax1, x1, eps) && feq(ay1, y1, eps));
}

int main(void)
{
	NVGcontext* ctx = make_ctx();
	int fontR, fontB;
	int idxA, idxSpace, idxQ, cp;
	NVGvertex qa[6], qb[6], ta[6];
	float ex0, ey0, ex1, ey1;
	int i;

	assert(ctx != NULL);
	fontR = nvgCreateFont(ctx, "roboto", "example/Roboto-Regular.ttf");
	fontB = nvgCreateFont(ctx, "roboto-bold", "example/Roboto-Bold.ttf");
	assert(fontR >= 0 && fontB >= 0 && fontR != fontB);
	load_stb(&g_stb, "example/Roboto-Regular.ttf");
	load_stb(&g_stbBold, "example/Roboto-Bold.ttf");

	idxA = stbtt_FindGlyphIndex(&g_stb, 'A');
	idxSpace = stbtt_FindGlyphIndex(&g_stb, ' ');
	idxQ = stbtt_FindGlyphIndex(&g_stb, 'Q');
	assert(idxA > 0 && idxSpace > 0 && idxQ > 0);
	assert(idxA < g_stbBold.numGlyphs);
	assert(!stbtt_IsGlyphEmpty(&g_stb, idxA));
	assert(stbtt_IsGlyphEmpty(&g_stb, idxSpace));
	assert(!stbtt_IsGlyphEmpty(&g_stb, idxQ));

	nvgBeginFrame(ctx, 160, 80, 1.0f);
	nvgFontSize(ctx, 24.0f);

	/* --- 1. fractional origins: exact translation, outline-derived quad,
	        and alpha/scissor/compositing through renderTriangles --- */
	{
		NVGpositionedGlyph g1 = { fontR, (unsigned int)idxA, 10.0f, 40.0f };
		NVGpositionedGlyph g2 = { fontR, (unsigned int)idxA, 10.5f, 40.25f };
		rec_reset();
		nvgScissor(ctx, 0, 0, 80, 60);
		nvgFillColor(ctx, nvgRGBAf(1, 1, 1, 0.5f));
		nvgGlobalAlpha(ctx, 0.4f);
		assert(nvgGlyphRun(ctx, &g1, 1) == 1);
		assert(g_nverts == 6 && g_drawcalls == 1 && g_updates > 0);
		assert(feq(g_alpha, 0.2f, 1e-6f));       /* 0.5 fill * 0.4 global */
		assert(g_scissorExtent0 == 40.0f);
		memcpy(qa, g_verts, sizeof(qa));
		expected_quad(idxA, 24.0f, 1.0f, 10.0f, 40.0f, &ex0, &ey0, &ex1, &ey1);
		assert_quad_matches(g_verts, ex0, ey0, ex1, ey1, 1e-4f);
		nvgGlobalAlpha(ctx, 1.0f);
		nvgResetScissor(ctx);
		nvgFillColor(ctx, nvgRGBAf(1, 1, 1, 1));

		/* same glyph, fractional origin: quad must translate exactly */
		g_nverts = 0; g_drawcalls = 0;
		assert(nvgGlyphRun(ctx, &g2, 1) == 1);
		assert(g_nverts == 6 && g_drawcalls == 1);
		for (i = 0; i < 6; i++) {
			assert(feq(g_verts[i].x - qa[i].x, 0.5f, 1e-4f));
			assert(feq(g_verts[i].y - qa[i].y, 0.25f, 1e-4f));
			assert(g_verts[i].u == qa[i].u && g_verts[i].v == qa[i].v); /* same cache entry */
		}
		expected_quad(idxA, 24.0f, 1.0f, 10.5f, 40.25f, &ex0, &ey0, &ex1, &ey1);
		assert_quad_matches(g_verts, ex0, ey0, ex1, ey1, 1e-4f);
		printf("1. fractional origin translation ok\n");
	}

	/* --- 2. same glyph index in two different fonts must not be confused --- */
	{
		NVGpositionedGlyph r1 = { fontR, (unsigned int)idxA, 10.0f, 40.0f };
		NVGpositionedGlyph b1 = { fontB, (unsigned int)idxA, 10.0f, 40.0f };
		rec_reset();
		assert(nvgGlyphRun(ctx, &r1, 1) == 1);
		assert(g_nverts == 6);
		memcpy(qa, g_verts, sizeof(qa));
		g_nverts = 0;
		assert(nvgGlyphRun(ctx, &b1, 1) == 1);
		assert(g_nverts == 6);
		memcpy(qb, g_verts, sizeof(qb));
		assert(memcmp(qa, qb, sizeof(qa)) != 0); /* different font, different quad/uv */
		g_nverts = 0;
		assert(nvgGlyphRun(ctx, &r1, 1) == 1); /* alternating back must hit the same cache entry */
		assert(g_nverts == 6);
		assert(memcmp(qa, g_verts, sizeof(qa)) == 0);
		printf("2. two fonts same index ok\n");
	}

	/* --- 3. codepoint (string path) vs glyph index of the same numeric value --- */
	{
		/* find a codepoint whose numeric value, used as a raw glyph index, has
		   a different outline box than the glyph the cmap maps it to */
		float s = stbtt_ScaleForMappingEmToPixels(&g_stb, 24.0f);
		cp = 65; /* 'A' */
		for (; cp < 200 && cp < g_stb.numGlyphs; cp++) {
			int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0, cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
			int viaCmap = stbtt_FindGlyphIndex(&g_stb, cp);
			if (stbtt_IsGlyphEmpty(&g_stb, cp)) continue;
			stbtt_GetGlyphBitmapBox(&g_stb, cp, s, s, &bx0, &by0, &bx1, &by1);
			stbtt_GetGlyphBitmapBox(&g_stb, viaCmap, s, s, &cx0, &cy0, &cx1, &cy1);
			if (bx0 != cx0 || by0 != cy0 || bx1 != cx1 || by1 != cy1) break;
		}
		assert(cp < 200 && cp < g_stb.numGlyphs);

		rec_reset();
		nvgFontFaceId(ctx, fontR);
		{
			char str[2];
			str[0] = (char)cp;
			str[1] = '\0';
			assert(nvgText(ctx, 10.0f, 40.0f, str, NULL) > 10.0f);
		}
		assert(g_nverts == 6);
		memcpy(ta, g_verts, sizeof(ta));

		/* glyph run with glyphIndex == codepoint value: must render the real
		   glyph at that index, not the cached codepoint entry */
		g_nverts = 0;
		{
			NVGpositionedGlyph g = { fontR, (unsigned int)cp, 10.0f, 40.0f };
			assert(nvgGlyphRun(ctx, &g, 1) == 1);
		}
		assert(g_nverts == 6);
		expected_quad(cp, 24.0f, 1.0f, 10.0f, 40.0f, &ex0, &ey0, &ex1, &ey1);
		assert_quad_matches(g_verts, ex0, ey0, ex1, ey1, 1e-4f);

		/* string path cache entry must be unaffected */
		g_nverts = 0;
		{
			char str[2];
			str[0] = (char)cp;
			str[1] = '\0';
			assert(nvgText(ctx, 10.0f, 40.0f, str, NULL) > 10.0f);
		}
		assert(g_nverts == 6);
		assert(memcmp(ta, g_verts, sizeof(ta)) == 0);
		printf("3. codepoint/index cache separation ok (codepoint %d)\n", cp);
	}

	/* --- 4. rotation and reflection: quads enter the canvas transform,
	        winding and UV direction match the string entry point --- */
	{
		NVGpositionedGlyph g1 = { fontR, (unsigned int)idxA, 10.0f, 40.0f };
		float b[4], xf[6];
		float px[4], py[4];

		/* rotation (not flipped) */
		rec_reset();
		nvgSave(ctx);
		nvgTranslate(ctx, 50.0f, 50.0f);
		nvgRotate(ctx, 0.7f);
		assert(nvgGlyphRun(ctx, &g1, 1) == 1);
		assert(g_nverts == 6);
		assert(nvgGlyphRunBounds(ctx, &g1, 1, b) == 1); /* local quad, no transform applied */
		nvgCurrentTransform(ctx, xf);
		nvgTransformPoint(&px[0], &py[0], xf, b[0], b[1]);
		nvgTransformPoint(&px[1], &py[1], xf, b[2], b[1]);
		nvgTransformPoint(&px[2], &py[2], xf, b[2], b[3]);
		nvgTransformPoint(&px[3], &py[3], xf, b[0], b[3]);
		/* triangle corner order: (0,2,1) (0,3,2) */
		assert(feq(g_verts[0].x, px[0], 1e-3f) && feq(g_verts[0].y, py[0], 1e-3f));
		assert(feq(g_verts[1].x, px[2], 1e-3f) && feq(g_verts[1].y, py[2], 1e-3f));
		assert(feq(g_verts[2].x, px[1], 1e-3f) && feq(g_verts[2].y, py[1], 1e-3f));
		assert(feq(g_verts[3].x, px[0], 1e-3f) && feq(g_verts[3].y, py[0], 1e-3f));
		assert(feq(g_verts[4].x, px[3], 1e-3f) && feq(g_verts[4].y, py[3], 1e-3f));
		assert(feq(g_verts[5].x, px[2], 1e-3f) && feq(g_verts[5].y, py[2], 1e-3f));
		nvgRestore(ctx);

		/* reflection: positions must match the string entry point exactly,
		   UV direction pattern must match too */
		rec_reset();
		nvgSave(ctx);
		nvgTranslate(ctx, 80.0f, 0.0f);
		nvgScale(ctx, -1.0f, 1.0f);
		assert(nvgGlyphRun(ctx, &g1, 1) == 1);
		assert(g_nverts == 6);
		memcpy(qa, g_verts, sizeof(qa));
		g_nverts = 0;
		nvgFontFaceId(ctx, fontR);
		assert(nvgText(ctx, 10.0f, 40.0f, "A", NULL) != 0.0f);
		assert(g_nverts == 6);
		for (i = 0; i < 6; i++) {
			float duRun, duTxt, dvRun, dvTxt;
			assert(feq(g_verts[i].x, qa[i].x, 1e-4f));
			assert(feq(g_verts[i].y, qa[i].y, 1e-4f));
			duRun = qa[i].u - qa[0].u; duTxt = g_verts[i].u - g_verts[0].u;
			dvRun = qa[i].v - qa[0].v; dvTxt = g_verts[i].v - g_verts[0].v;
			assert((duRun > 0) == (duTxt > 0) && (duRun < 0) == (duTxt < 0));
			assert((dvRun > 0) == (dvTxt > 0) && (dvRun < 0) == (dvTxt < 0));
		}
		nvgRestore(ctx);
		printf("4. rotation/reflection ok\n");
	}

	/* --- 6. measurement: no bitmap, no texture upload, matches drawn quads --- */
	{
		NVGpositionedGlyph gq = { fontR, (unsigned int)idxQ, 7.5f, 33.25f };
		float b[4] = { 1, 2, 3, 4 };
		nvgFontSize(ctx, 26.0f); /* fresh size: glyph not cached yet */
		rec_reset();
		assert(nvgGlyphRunBounds(ctx, &gq, 1, b) == 1);
		assert(g_updates == 0 && g_drawcalls == 0 && g_nverts == 0); /* no texture events, no drawing */
		expected_quad(idxQ, 26.0f, 1.0f, 7.5f, 33.25f, &ex0, &ey0, &ex1, &ey1);
		assert(feq(b[0], ex0, 1e-4f) && feq(b[1], ey0, 1e-4f));
		assert(feq(b[2], ex1, 1e-4f) && feq(b[3], ey1, 1e-4f));
		/* drawing afterwards uploads the bitmap and submits the same local quad */
		assert(nvgGlyphRun(ctx, &gq, 1) == 1);
		assert(g_updates > 0 && g_nverts == 6);
		assert_quad_matches(g_verts, b[0], b[1], b[2], b[3], 1e-4f);
		nvgFontSize(ctx, 24.0f);
		printf("6. measurement texture events ok\n");
	}

	/* --- 7. empty outline: no triangles, following glyph unaffected --- */
	{
		NVGpositionedGlyph run2[2] = {
			{ fontR, (unsigned int)idxSpace, 10.0f, 40.0f },
			{ fontR, (unsigned int)idxA, 30.0f, 40.0f },
		};
		NVGpositionedGlyph onlyA = { fontR, (unsigned int)idxA, 30.0f, 40.0f };
		float b1[4], b2[4];
		rec_reset();
		assert(nvgGlyphRun(ctx, run2, 2) == 1);
		assert(g_nverts == 6); /* only the 'A' quad, empty outline emits nothing */
		expected_quad(idxA, 24.0f, 1.0f, 30.0f, 40.0f, &ex0, &ey0, &ex1, &ey1);
		assert_quad_matches(g_verts, ex0, ey0, ex1, ey1, 1e-4f);
		assert(nvgGlyphRunBounds(ctx, run2, 2, b1) == 1);
		assert(nvgGlyphRunBounds(ctx, &onlyA, 1, b2) == 1);
		assert(b1[0] == b2[0] && b1[1] == b2[1] && b1[2] == b2[2] && b1[3] == b2[3]);
		printf("7. empty outline ok\n");
	}

	/* --- 8. invalid input: rejected before any draw or texture update --- */
	{
		NVGpositionedGlyph good = { fontR, (unsigned int)idxA, 10.0f, 40.0f };
		NVGpositionedGlyph badFont = { 999, (unsigned int)idxA, 10.0f, 40.0f };
		NVGpositionedGlyph badFont2 = { -1, (unsigned int)idxA, 10.0f, 40.0f };
		NVGpositionedGlyph badIdx = { fontR, (unsigned int)g_stb.numGlyphs + 5u, 10.0f, 40.0f };
		NVGpositionedGlyph nanX = { fontR, (unsigned int)idxA, NAN, 40.0f };
		NVGpositionedGlyph infY = { fontR, (unsigned int)idxA, 10.0f, INFINITY };
		NVGpositionedGlyph mix[2] = {
			{ fontR, (unsigned int)idxA, 10.0f, 40.0f },
			{ fontR, (unsigned int)g_stb.numGlyphs + 5u, 20.0f, 40.0f },
		};
		float b[4];
		rec_reset();
		assert(nvgGlyphRun(ctx, &good, -1) == 0);
		assert(nvgGlyphRun(ctx, NULL, 1) == 0);
		assert(nvgGlyphRun(ctx, &badFont, 1) == 0);
		assert(nvgGlyphRun(ctx, &badFont2, 1) == 0);
		assert(nvgGlyphRun(ctx, &badIdx, 1) == 0);
		assert(nvgGlyphRun(ctx, &nanX, 1) == 0);
		assert(nvgGlyphRun(ctx, &infY, 1) == 0);
		assert(nvgGlyphRun(ctx, mix, 2) == 0); /* rejected before submitting anything */
		assert(g_nverts == 0 && g_drawcalls == 0 && g_updates == 0);

		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, &good, -1, b) == 0);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, NULL, 1, b) == 0);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, &badFont, 1, b) == 0);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, &badIdx, 1, b) == 0);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, &nanX, 1, b) == 0);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		assert(g_nverts == 0 && g_drawcalls == 0 && g_updates == 0);

		/* empty sequence succeeds with zero bounds */
		assert(nvgGlyphRun(ctx, NULL, 0) == 1);
		assert(nvgGlyphRun(ctx, &good, 0) == 1);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, NULL, 0, b) == 1);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		b[0] = b[1] = b[2] = b[3] = 9.0f;
		assert(nvgGlyphRunBounds(ctx, &good, 0, b) == 1);
		assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
		assert(g_nverts == 0 && g_drawcalls == 0 && g_updates == 0);
		printf("8. invalid input ok\n");
	}

	nvgEndFrame(ctx);

	/* --- 5. different device pixel ratio: same local quad, fresh bitmap --- */
	rec_reset();
	nvgBeginFrame(ctx, 160, 80, 2.0f);
	nvgFontSize(ctx, 24.0f); /* frame begin resets the state */
	{
		NVGpositionedGlyph g1 = { fontR, (unsigned int)idxA, 10.0f, 40.0f };
		float ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
		assert(nvgGlyphRun(ctx, &g1, 1) == 1);
		assert(g_nverts == 6 && g_updates > 0); /* re-rasterized at the new size */
		quad_bounds(g_verts, &ax0, &ay0, &ax1, &ay1);
		/* exact match against the outline metrics at 2x rasterization */
		expected_quad(idxA, 24.0f, 2.0f, 10.0f, 40.0f, &ex0, &ey0, &ex1, &ey1);
		assert(feq(ax0, ex0, 1e-4f) && feq(ay0, ey0, 1e-4f));
		assert(feq(ax1, ex1, 1e-4f) && feq(ay1, ey1, 1e-4f));
		/* approximately device independent: within a device pixel plus outline
		   rounding of the dpr=1 quad */
		expected_quad(idxA, 24.0f, 1.0f, 10.0f, 40.0f, &bx0, &by0, &bx1, &by1);
		assert(feq(ax0, bx0, 1.05f) && feq(ay0, by0, 1.05f));
		assert(feq(ax1, bx1, 1.05f) && feq(ay1, by1, 1.05f));
	}
	nvgEndFrame(ctx);
	printf("5. device pixel ratio ok\n");

	nvgDeleteInternal(ctx);
	printf("check_glyph_run: all tests passed\n");
	return 0;
}
