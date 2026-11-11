/* Regression tests for the fixed-size multi-page glyph atlas.
 * Local test code, zlib license. Pure C, no windows, no extra dependencies.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Force small vertex buffers so page-change and buffer-full batch splits
 * are exercised by every draw call. */
#ifndef FONS_VERTEX_COUNT
#define FONS_VERTEX_COUNT 24
#endif

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"
#include "gl_spy.h"
#define GLFONTSTASH_IMPLEMENTATION
#include "glfontstash.h"
#include "case.h"

#define FONT_PATH "example/DroidSansJapanese.ttf"
static const char PRESSURE[] = "日本語文字漢東京大阪京都北海道青空山川海花月星金木水火土石";
#define MAX_PAGES 64

/* ------------------------------------------------------------------ */
/* CPU recording backend for the paged interface.                      */
/* ------------------------------------------------------------------ */

typedef struct {
	int alive, w, h, updates;
	int lastRect[4];
	unsigned char* pixels;
} PPage;

typedef struct {
	int page;
	int nverts;
	int updatesSnapshot;
	float verts[FONS_VERTEX_COUNT*2];
	float uv[FONS_VERTEX_COUNT*2];
} PDraw;

typedef struct Recorder {
	int w, h, maxPages;
	PPage pages[MAX_PAGES];
	int createCalls;
	int failCreatesLeft;   /* next N create-page calls fail */
	int deletePageCalls;
	int deleteOrder[128];
	int deleteOrderN;
	int deleteShared;
	int updates;
	int updateOrder[256];
	int updateOrderN;
	PDraw draws[256];
	int ndraws;
	int full;
} Recorder;

static void rec_error(void* uptr, int error, int val)
{
	Recorder* r = (Recorder*)uptr;
	(void)val;
	assert(error == FONS_ATLAS_FULL);
	r->full++;
}

static int rec_create_page(void* uptr, int page, int w, int h)
{
	Recorder* r = (Recorder*)uptr;
	r->createCalls++;
	if (r->failCreatesLeft > 0) {
		r->failCreatesLeft--;
		return 0;
	}
	assert(page >= 0 && page < r->maxPages);
	assert(!r->pages[page].alive);
	r->pages[page].alive = 1;
	r->pages[page].w = w;
	r->pages[page].h = h;
	r->pages[page].updates = 0;
	r->pages[page].pixels = (unsigned char*)calloc((size_t)w*h, 1);
	assert(r->pages[page].pixels != NULL);
	return 1;
}

static void rec_update_page(void* uptr, int page, int* rect, const unsigned char* data)
{
	Recorder* r = (Recorder*)uptr;
	int y;
	PPage* p;
	assert(page >= 0 && page < r->maxPages && r->pages[page].alive);
	assert(rect[0] >= 0 && rect[1] >= 0);
	assert(rect[2] <= r->w && rect[3] <= r->h);
	assert(rect[0] < rect[2] && rect[1] < rect[3]);
	p = &r->pages[page];
	for (y = rect[1]; y < rect[3]; y++)
		memcpy(p->pixels + y*r->w + rect[0],
			   data + y*r->w + rect[0], (size_t)(rect[2]-rect[0]));
	p->updates++;
	memcpy(p->lastRect, rect, sizeof(p->lastRect));
	r->updates++;
	assert(r->updateOrderN < (int)(sizeof(r->updateOrder)/sizeof(r->updateOrder[0])));
	r->updateOrder[r->updateOrderN++] = page;
}

static void rec_draw_page(void* uptr, int page, const float* verts, const float* uv,
						  const unsigned int* colors, int nverts)
{
	Recorder* r = (Recorder*)uptr;
	int i, ink = 0;
	PPage* p;
	PDraw* d;
	(void)colors;
	assert(page >= 0 && page < r->maxPages && r->pages[page].alive);
	assert(nverts > 0 && nverts % 6 == 0 && nverts <= FONS_VERTEX_COUNT);
	assert(r->ndraws < (int)(sizeof(r->draws)/sizeof(r->draws[0])));
	p = &r->pages[page];
	/* A draw for a page must follow at least one pixel upload to that page. */
	assert(p->updates > 0);
	for (i = 0; i < p->w*p->h; i++) ink += p->pixels[i] != 0;
	assert(ink > 4);
	d = &r->draws[r->ndraws++];
	d->page = page;
	d->nverts = nverts;
	d->updatesSnapshot = r->updates;
	memcpy(d->verts, verts, (size_t)nverts*2*sizeof(float));
	memcpy(d->uv, uv, (size_t)nverts*2*sizeof(float));
	for (i = 0; i < nverts*2; i++) {
		assert(isfinite(verts[i]));
		assert(uv[i] >= 0.0f && uv[i] <= 1.0f);
	}
}

static void rec_delete_page(void* uptr, int page)
{
	Recorder* r = (Recorder*)uptr;
	assert(page >= 0 && page < r->maxPages && r->pages[page].alive);
	free(r->pages[page].pixels);
	memset(&r->pages[page], 0, sizeof(r->pages[page]));
	r->deletePageCalls++;
	assert(r->deleteOrderN < (int)(sizeof(r->deleteOrder)/sizeof(r->deleteOrder[0])));
	r->deleteOrder[r->deleteOrderN++] = page;
}

static void rec_delete_shared(void* uptr)
{
	Recorder* r = (Recorder*)uptr;
	r->deleteShared++;
}

static FONScontext* make_paged(Recorder* r, int w, int h, int maxPages, int flags)
{
	FONSpagedParams p;
	memset(r, 0, sizeof(*r));
	memset(&p, 0, sizeof(p));
	r->w = w; r->h = h; r->maxPages = maxPages;
	p.width = w; p.height = h; p.maxPages = maxPages;
	p.flags = (unsigned char)flags;
	p.userPtr = r;
	p.renderCreatePage = rec_create_page;
	p.renderUpdatePage = rec_update_page;
	p.renderDrawPage = rec_draw_page;
	p.renderDeletePage = rec_delete_page;
	p.renderDelete = rec_delete_shared;
	return fonsCreateInternalPaged(&p);
}

/* ------------------------------------------------------------------ */
/* Single page reference backend with ample capacity.                  */
/* ------------------------------------------------------------------ */

typedef struct {
	int w, h, updates, draws;
	unsigned char* pixels;
	int nverts;
	float verts[8192];
	float uv[8192];
} SRef;

static int sref_create(void* uptr, int w, int h)
{
	SRef* s = (SRef*)uptr;
	s->w = w; s->h = h;
	s->pixels = (unsigned char*)calloc((size_t)w*h, 1);
	return s->pixels != NULL;
}
static int sref_resize(void* uptr, int w, int h)
{
	SRef* s = (SRef*)uptr;
	(void)uptr; (void)w; (void)h;
	free(s->pixels);
	s->pixels = NULL;
	return 0;
}
static void sref_update(void* uptr, int* rect, const unsigned char* data)
{
	SRef* s = (SRef*)uptr;
	int y;
	for (y = rect[1]; y < rect[3]; y++)
		memcpy(s->pixels + y*s->w + rect[0],
			   data + y*s->w + rect[0], (size_t)(rect[2]-rect[0]));
	s->updates++;
}
static void sref_draw(void* uptr, const float* v, const float* uv,
					  const unsigned int* colors, int n)
{
	SRef* s = (SRef*)uptr;
	(void)colors;
	assert((s->nverts+n)*2 <= (int)(sizeof(s->verts)/sizeof(s->verts[0])));
	memcpy(s->verts + s->nverts*2, v, (size_t)n*2*sizeof(float));
	memcpy(s->uv + s->nverts*2, uv, (size_t)n*2*sizeof(float));
	s->nverts += n;
	s->draws++;
}
static void sref_delete(void* uptr)
{
	SRef* s = (SRef*)uptr;
	free(s->pixels);
	s->pixels = NULL;
}

static FONScontext* make_single(SRef* s, int w, int h, int flags)
{
	FONSparams p;
	memset(s, 0, sizeof(*s));
	memset(&p, 0, sizeof(p));
	p.width = w; p.height = h; p.flags = (unsigned char)flags; p.userPtr = s;
	p.renderCreate = sref_create;
	p.renderResize = sref_resize;
	p.renderUpdate = sref_update;
	p.renderDraw = sref_draw;
	p.renderDelete = sref_delete;
	return fonsCreateInternal(&p);
}

static int add_font(FONScontext* s)
{
	int f = fonsAddFont(s, "jp", FONT_PATH);
	assert(f >= 0);
	fonsSetFont(s, f);
	fonsSetSize(s, 24.0f);
	fonsSetColor(s, 0xffffffff);
	return f;
}

static int put_utf8(unsigned int cp, char* out)
{
	if (cp < 0x80) { out[0] = (char)cp; return 1; }
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 63));
		return 2;
	}
	out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
	out[1] = (char)(0x80 | ((cp >> 6) & 63));
	out[2] = (char)(0x80 | (cp & 63));
	return 3;
}

static int floats_eq(const float* a, const float* b, int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (fabsf(a[i]-b[i]) > 1e-5f) return 0;
	return 1;
}

/* Flatten all recorded paged draw batches into one vertex stream. */
static void flatten(Recorder* r, float* verts, float* uv, int* nverts)
{
	int i, k, off = 0;
	for (i = 0; i < r->ndraws; i++) {
		for (k = 0; k < r->draws[i].nverts*2; k++) {
			verts[off+k] = r->draws[i].verts[k];
			uv[off+k] = r->draws[i].uv[k];
		}
		off += r->draws[i].nverts*2;
	}
	*nverts = off / 2;
}

static int count_codepoints(const char* str)
{
	unsigned int state = 0, cp = 0;
	int n = 0;
	for (; *str; ++str)
		if (!fons__decutf8(&state, &cp, (unsigned char)*str)) n++;
	return n;
}

/* ================================================================ */
/* Case 1: measurement takes no residency, layout matches reference  */
/* ================================================================ */

static int case_layout_without_residency(void)
{
	Recorder rec;
	SRef ref;
	FONScontext* pg;
	FONScontext* sg;
	float pb[4], sb[4], padv, sadv, pend, send;
	int fontp, fonts;
	const int flags[2] = { FONS_ZERO_TOPLEFT, FONS_ZERO_BOTTOMLEFT };
	int fi;

	for (fi = 0; fi < 2; fi++) {
		float blurv[2] = { 0.0f, 2.0f };
		int bi;
		for (bi = 0; bi < 2; bi++) {
			pg = make_paged(&rec, 64, 64, 16, flags[fi]);
			sg = make_single(&ref, 1024, 1024, flags[fi]);
			assert(pg && sg);
			fontp = add_font(pg);
			fonts = add_font(sg);
			(void)fontp; (void)fonts;
			fonsSetBlur(pg, blurv[bi]);
			fonsSetBlur(sg, blurv[bi]);
			fonsSetErrorCallback(pg, rec_error, &rec);

			/* Pure measurement: no rasterization, no pages, no events. */
			padv = fonsTextBounds(pg, 5, 40, PRESSURE, NULL, pb);
			assert(rec.createCalls == 1);
			assert(rec.updates == 0 && rec.ndraws == 0);
			assert(fonsGetAtlasPageCount(pg) == 1);

			/* Center and right pre-measurement stay event free as well. */
			fonsSetAlign(pg, FONS_ALIGN_CENTER | FONS_ALIGN_BASELINE);
			(void)fonsTextBounds(pg, 200, 40, PRESSURE, NULL, pb);
			fonsSetAlign(pg, FONS_ALIGN_RIGHT | FONS_ALIGN_BASELINE);
			(void)fonsTextBounds(pg, 400, 40, PRESSURE, NULL, pb);
			fonsSetAlign(pg, FONS_ALIGN_LEFT | FONS_ALIGN_BASELINE);
			assert(rec.updates == 0 && rec.ndraws == 0 &&
				   fonsGetAtlasPageCount(pg) == 1);

			/* Bounds and advance match the roomy single page instance. */
			sadv = fonsTextBounds(sg, 5, 40, PRESSURE, NULL, sb);
			padv = fonsTextBounds(pg, 5, 40, PRESSURE, NULL, pb);
			assert(fabsf(padv - sadv) < 1e-5f);
			assert(floats_eq(pb, sb, 4));

			/* First real draw after measurement rasterizes everything. */
			pend = fonsDrawText(pg, 5, 40, PRESSURE, NULL);
			send = fonsDrawText(sg, 5, 40, PRESSURE, NULL);
			assert(fabsf(pend - send) < 1e-5f);
			assert(rec.updates > 0 && rec.ndraws > 0);
			assert(fonsGetAtlasPageCount(pg) >= 2);

			/* Vertex positions must match the reference quad for quad. */
			{
				float pv[8192], puv[8192];
				int pn;
				flatten(&rec, pv, puv, &pn);
				assert(pn == ref.nverts);
				assert(floats_eq(pv, ref.verts, pn*2));
			}

			fonsDeleteInternal(pg);
			fonsDeleteInternal(sg);
		}
	}

	/* Empty outline (space) keeps the advance but submits no triangles. */
	{
		pg = make_paged(&rec, 64, 64, 16, FONS_ZERO_TOPLEFT);
		sg = make_single(&ref, 1024, 1024, FONS_ZERO_TOPLEFT);
		assert(pg && sg);
		add_font(pg); add_font(sg);
		fonsSetErrorCallback(pg, rec_error, &rec);
		pend = fonsDrawText(pg, 0, 40, "日 本", NULL);
		send = fonsDrawText(sg, 0, 40, "日 本", NULL);
		assert(fabsf(pend - send) < 1e-5f);
		/* Reference emits a blank quad for the space, the paged stash does not. */
		{
			float pv[8192], puv[8192];
			int pn;
			flatten(&rec, pv, puv, &pn);
			assert(ref.nverts == 18 && pn == 12);
			assert(pv[0] == ref.verts[0]);
		}
		fonsDeleteInternal(pg);
		fonsDeleteInternal(sg);
	}

	/* Center and right aligned draws land on the same vertices as the
	 * roomy single page reference (pre-measurement stays event free). */
	{
		const int aligns[2] = {
			FONS_ALIGN_CENTER | FONS_ALIGN_BASELINE,
			FONS_ALIGN_RIGHT | FONS_ALIGN_BASELINE
		};
		int ai;
		for (ai = 0; ai < 2; ai++) {
			pg = make_paged(&rec, 64, 64, 16, FONS_ZERO_TOPLEFT);
			sg = make_single(&ref, 1024, 1024, FONS_ZERO_TOPLEFT);
			assert(pg && sg);
			add_font(pg); add_font(sg);
			fonsSetErrorCallback(pg, rec_error, &rec);
			fonsSetAlign(pg, aligns[ai]);
			fonsSetAlign(sg, aligns[ai]);
			/* Pre-measurement on the paged stash creates no pages. */
			(void)fonsTextBounds(pg, 300, 40, PRESSURE, NULL, NULL);
			assert(fonsGetAtlasPageCount(pg) == 1 && rec.updates == 0);
			fonsDrawText(pg, 300, 40, PRESSURE, NULL);
			fonsDrawText(sg, 300, 40, PRESSURE, NULL);
			{
				float pv[8192], puv[8192];
				int pn;
				flatten(&rec, pv, puv, &pn);
				assert(pn == ref.nverts);
				assert(floats_eq(pv, ref.verts, pn*2));
			}
			fonsDeleteInternal(pg);
			fonsDeleteInternal(sg);
		}
	}

	return 0;
}

/* ================================================================ */
/* Case 2: submission order, batching and per-page pixel coverage    */
/* ================================================================ */

static int case_page_order_and_pixels(void)
{
	Recorder rec;
	FONScontext* pg;
	FONStextIter it;
	FONSquad q;
	unsigned int cp0 = 0, cp1 = 0;
	char triple[16];
	int n, i, updatesBefore, pagesBefore;
	float flatV[8192], flatU[8192];
	int fn;

	pg = make_paged(&rec, 64, 64, 16, FONS_ZERO_TOPLEFT);
	assert(pg);
	add_font(pg);
	fonsSetErrorCallback(pg, rec_error, &rec);

	/* Fill several pages first. */
	fonsDrawText(pg, 2, 40, PRESSURE, NULL);
	assert(fonsGetAtlasPageCount(pg) >= 3);
	assert(rec.full == 0);

	/* Pick a codepoint resident on page 0 and one on page 1. */
	assert(fonsTextIterInit(pg, &it, 0, 40, PRESSURE, NULL));
	while (fonsTextIterNext(pg, &it, &q)) {
		if (it.page == 0 && cp0 == 0) cp0 = it.codepoint;
		if (it.page == 1 && cp1 == 0) cp1 = it.codepoint;
	}
	assert(cp0 && cp1);

	/* Clear the event log, then draw page0, page1, page0 glyphs in order. */
	rec.ndraws = 0;
	rec.updates = 0;
	n = 0;
	n += put_utf8(cp0, triple+n);
	n += put_utf8(cp1, triple+n);
	n += put_utf8(cp0, triple+n);
	triple[n] = '\0';

	updatesBefore = rec.updates;
	fonsDrawText(pg, 2, 80, triple, NULL);

	/* All three glyphs are cached: no re-rasterize, no re-upload. */
	assert(rec.updates == updatesBefore);
	/* Submission order follows the input, not the texture id: 0, 1, 0. */
	assert(rec.ndraws == 3);
	assert(rec.draws[0].page == 0 && rec.draws[1].page == 1 &&
		   rec.draws[2].page == 0);
	/* Each batch carries exactly one quad and belongs to a single page. */
	for (i = 0; i < rec.ndraws; i++)
		assert(rec.draws[i].nverts == 6);

	/* Every drawn quad covers real ink on its own page through the
	 * per-page texture query. */
	flatten(&rec, flatV, flatU, &fn);
	assert(fn == 18);
	for (i = 0; i < 3; i++) {
		int page = rec.draws[i].page;
		int w, h, x0, y0, x1, y1, xx, yy, ink = 0;
		const unsigned char* data;
		float* uv = rec.draws[i].uv;
		data = fonsGetTextureDataPage(pg, page, &w, &h);
		assert(data && w == 64 && h == 64);
		/* Vertex 0 carries (s0,t0), vertex 1 carries (s1,t1). */
		x0 = (int)(uv[0]*w);
		y0 = (int)(uv[1]*h);
		x1 = (int)(uv[2]*w + 0.999f);
		y1 = (int)(uv[3]*h + 0.999f);
		assert(x0 >= 0 && y0 >= 0 && x1 <= w && y1 <= h && x1 > x0 && y1 > y0);
		for (yy = y0; yy < y1; yy++)
			for (xx = x0; xx < x1; xx++)
				ink += data[yy*w+xx] != 0;
		assert(ink > 0);
	}

	/* Buffer-full ordering: redraw the whole cached string. With
	 * FONS_VERTEX_COUNT small, batches must split both on page change and
	 * on capacity while keeping exact input order. Reconstruct the expected
	 * batch (page, vertex-count) sequence from the iterator's per-glyph
	 * pages and compare it with the recorded callbacks. */
	{
		int glyphPage[64], ncp2 = 0, k, bi = 0, curPage = -1, curVerts = 0;
		assert(fonsTextIterInit(pg, &it, 0, 40, PRESSURE, NULL));
		while (fonsTextIterNext(pg, &it, &q)) {
			assert(it.page >= 0);
			assert(ncp2 < 64);
			glyphPage[ncp2++] = it.page;
		}
		rec.ndraws = 0;
		fonsDrawText(pg, 9, 70, PRESSURE, NULL);
		assert(rec.ndraws > 0);
		for (k = 0; k < ncp2; k++) {
			if (curPage != glyphPage[k] || curVerts + 6 > FONS_VERTEX_COUNT) {
				if (curVerts > 0) {
					assert(bi < rec.ndraws);
					assert(rec.draws[bi].page == curPage);
					assert(rec.draws[bi].nverts == curVerts);
					bi++;
				}
				curPage = glyphPage[k];
				curVerts = 0;
			}
			curVerts += 6;
		}
		if (curVerts > 0) {
			assert(bi < rec.ndraws);
			assert(rec.draws[bi].page == curPage);
			assert(rec.draws[bi].nverts == curVerts);
			bi++;
		}
		assert(bi == rec.ndraws);
		/* Every callback batch stays within one page and the vertex cap. */
		for (k = 0; k < rec.ndraws; k++)
			assert(rec.draws[k].nverts <= FONS_VERTEX_COUNT &&
				   rec.draws[k].nverts % 6 == 0);
	}

	/* Invalid pages create nothing and consume no other page's dirt. */
	{
		int dirty[4], pw, ph;
		assert(fonsGetTextureDataPage(pg, 99, &pw, &ph) == NULL && pw == 0 && ph == 0);
		assert(fonsGetTextureDataPage(pg, -1, &pw, &ph) == NULL && pw == 0 && ph == 0);
		assert(fonsValidateTexturePage(pg, 99, dirty) == 0);
		/* Post-draw dirt has already been consumed by the draw flush. */
		assert(fonsValidateTexturePage(pg, 0, dirty) == 0);
		assert(fonsValidateTexturePage(pg, 1, dirty) == 0);

		/* White-box pending dirt: page 1 gets a half-open dirty rect. The
		 * implementation struct is fully visible in this translation unit. */
		pg->pageDirtyRects[1][0] = 3;
		pg->pageDirtyRects[1][1] = 5;
		pg->pageDirtyRects[1][2] = 11;
		pg->pageDirtyRects[1][3] = 17;
		/* Querying another page leaves page 1's dirt untouched. */
		assert(fonsValidateTexturePage(pg, 99, dirty) == 0);
		assert(fonsValidateTexturePage(pg, 0, dirty) == 0);
		assert(fonsValidateTexturePage(pg, 1, dirty) == 1);
		assert(dirty[0] == 3 && dirty[1] == 5 && dirty[2] == 11 && dirty[3] == 17);
		/* Half-open rect consumed: second call reports nothing. */
		assert(fonsValidateTexturePage(pg, 1, dirty) == 0);
	}

	/* Legacy query aliases page 0. */
	{
		int w, h;
		const unsigned char* d0 = fonsGetTextureDataPage(pg, 0, &w, &h);
		const unsigned char* dl = fonsGetTextureData(pg, &w, &h);
		assert(d0 == dl && w == 64 && h == 64);
		fonsGetAtlasSize(pg, &w, &h);
		assert(w == 64 && h == 64);
	}

	pagesBefore = fonsGetAtlasPageCount(pg);
	/* Fixed page size: expand/reset refuse without callbacks or state change. */
	assert(fonsExpandAtlas(pg, 256, 256) == 0);
	assert(fonsResetAtlas(pg, 256, 256) == 0);
	assert(fonsGetAtlasPageCount(pg) == pagesBefore);
	assert(rec.full == 0);

	fonsDeleteInternal(pg);
	/* Every page deleted exactly once, in order, then shared cleanup once. */
	assert(rec.deletePageCalls == pagesBefore);
	assert(rec.deleteShared == 1);
	for (i = 0; i < pagesBefore; i++)
		assert(rec.deleteOrder[i] == i);
	return 0;
}

/* ================================================================ */
/* Case 3: cache reuse and independent iterators                     */
/* ================================================================ */

static int case_cache_and_iterators(void)
{
	Recorder rec;
	FONScontext* pg;
	FONStextIter a, b;
	FONSquad qa, qb;
	int i, updatesBefore, drawsBefore;
	float refx[64];
	int refpage[64];
	int ncp;

	pg = make_paged(&rec, 64, 64, 16, FONS_ZERO_TOPLEFT);
	assert(pg);
	add_font(pg);
	fonsSetErrorCallback(pg, rec_error, &rec);

	/* Measure first: nothing is resident yet. */
	(void)fonsTextBounds(pg, 0, 40, PRESSURE, NULL, NULL);
	assert(fonsGetAtlasPageCount(pg) == 1 && rec.updates == 0);

	/* Iterator over metrics-only glyphs: page -1, zeroed quad, correct pen. */
	ncp = count_codepoints(PRESSURE);
	assert(ncp < 64);
	assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
	i = 0;
	while (fonsTextIterNext(pg, &a, &qa)) {
		assert(a.page == -1);
		assert(qa.x0 == 0 && qa.y0 == 0 && qa.x1 == 0 && qa.y1 == 0 &&
			   qa.s0 == 0 && qa.t0 == 0 && qa.s1 == 0 && qa.t1 == 0);
		refx[i] = a.nextx;
		i++;
	}
	assert(i == ncp);

	/* First real draw materializes the bitmaps, metrics were already cached. */
	fonsDrawText(pg, 0, 40, PRESSURE, NULL);
	assert(rec.updates > 0 && fonsGetAtlasPageCount(pg) >= 2);

	/* After residency the iterator reports pages and non-zero quads, and the
	 * pen positions match the residency-free reference run exactly. */
	assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
	i = 0;
	while (fonsTextIterNext(pg, &a, &qa)) {
		assert(a.page >= 0);
		assert(qa.x1 > qa.x0 && qa.y1 != qa.y0);
		assert(fabsf(a.nextx - refx[i]) < 1e-5f);
		refpage[i] = a.page;
		i++;
	}
	assert(i == ncp);

	/* Cache reuse: redraw triggers neither rasterization nor uploads. */
	updatesBefore = rec.updates;
	drawsBefore = rec.ndraws;
	fonsDrawText(pg, 100, 40, PRESSURE, NULL);
	assert(rec.updates == updatesBefore);
	assert(rec.ndraws > drawsBefore);

	/* Stability: snapshot the first few resident glyphs (page, UV rect and
	 * the actual page pixels inside it), force more pages to be created, then
	 * confirm nothing about an existing glyph moved or changed. */
	{
		struct Snap { int page, w, h; float s0, t0, s1, t1; unsigned char px[64*64]; } snap[4];
		int ns = 0, k, xx, yy;
		assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
		while (fonsTextIterNext(pg, &a, &qa) && ns < 4) {
			int pw, ph;
			const unsigned char* data;
			if (a.page < 0) continue;
			data = fonsGetTextureDataPage(pg, a.page, &pw, &ph);
			assert(data && pw == 64 && ph == 64);
			snap[ns].page = a.page;
			snap[ns].s0 = qa.s0; snap[ns].t0 = qa.t0;
			snap[ns].s1 = qa.s1; snap[ns].t1 = qa.t1;
			snap[ns].w = (int)(qa.s1*pw + 0.999f) - (int)(qa.s0*pw + 0.5f);
			snap[ns].h = (int)(qa.t1*ph + 0.999f) - (int)(qa.t0*ph + 0.5f);
			{
				int rx0 = (int)(qa.s0*pw + 0.5f), ry0 = (int)(qa.t0*ph + 0.5f);
				for (yy = 0; yy < snap[ns].h; yy++)
					for (xx = 0; xx < snap[ns].w; xx++)
						snap[ns].px[yy*snap[ns].w+xx] = data[(ry0+yy)*pw + rx0+xx];
			}
			ns++;
		}
		assert(ns == 4);
		/* New size + full string packs more glyphs and creates more pages. */
		fonsSetSize(pg, 21.0f);
		fonsDrawText(pg, 0, 90, PRESSURE, NULL);
		fonsSetSize(pg, 24.0f);
		assert(fonsGetAtlasPageCount(pg) >= 5);
		assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
		k = 0;
		while (fonsTextIterNext(pg, &a, &qa) && k < ns) {
			int pw, ph;
			const unsigned char* data;
			if (a.page < 0) continue;
			assert(a.page == snap[k].page);
			assert(qa.s0 == snap[k].s0 && qa.t0 == snap[k].t0 &&
				   qa.s1 == snap[k].s1 && qa.t1 == snap[k].t1);
			data = fonsGetTextureDataPage(pg, a.page, &pw, &ph);
			{
				int rx0 = (int)(qa.s0*pw + 0.5f), ry0 = (int)(qa.t0*ph + 0.5f);
				for (yy = 0; yy < snap[k].h; yy++)
					for (xx = 0; xx < snap[k].w; xx++)
						assert(data[(ry0+yy)*pw + rx0+xx] ==
							   snap[k].px[yy*snap[k].w+xx]);
			}
			k++;
		}
		assert(k == ns);
	}

	/* Two iterators interleaved stay independent of each other. */
	assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
	assert(fonsTextIterInit(pg, &b, 7, 90, PRESSURE, NULL));
	{
		int ia = 0, ib = 0;
		/* Advance A twice, then B twice: each keeps its own pen. */
		assert(fonsTextIterNext(pg, &a, &qa));
		assert(a.page == refpage[0]);
		assert(fonsTextIterNext(pg, &a, &qa));
		assert(a.page == refpage[1]);
		assert(fonsTextIterNext(pg, &b, &qb));
		assert(qb.x0 >= 7.0f);
		assert(fonsTextIterNext(pg, &b, &qb));
		/* A's pen is unaffected by B's advancement. */
		assert(fabsf(a.nextx - refx[1]) < 1e-5f);
		ia = 2; ib = 2;
		while (fonsTextIterNext(pg, &a, &qa)) {
			assert(a.page == refpage[ia]);
			assert(fabsf(a.nextx - refx[ia]) < 1e-5f);
			ia++;
		}
		while (fonsTextIterNext(pg, &b, &qb))
			ib++;
		assert(ia == ncp && ib == ncp);
	}

	/* A draw switching pages mid-flight must not corrupt another iterator. */
	assert(fonsTextIterInit(pg, &a, 0, 40, PRESSURE, NULL));
	assert(fonsTextIterNext(pg, &a, &qa));
	fonsDrawText(pg, 300, 40, PRESSURE, NULL);
	assert(fonsTextIterNext(pg, &a, &qa));
	assert(fabsf(a.nextx - refx[1]) < 1e-5f);
	while (fonsTextIterNext(pg, &a, &qa))
		;
	assert(fabsf(a.nextx - refx[ncp-1]) < 1e-5f);

	/* A freshly introduced glyph is still reported as non-resident until drawn. */
	{
		FONStextIter c;
		FONSquad qc;
		fonsSetSize(pg, 15.0f);
		(void)fonsTextBounds(pg, 0, 40, "東", NULL, NULL);
		assert(fonsTextIterInit(pg, &c, 0, 40, "東", NULL));
		assert(fonsTextIterNext(pg, &c, &qc));
		assert(c.page == -1);
		assert(qc.x0 == 0 && qc.y0 == 0 && qc.x1 == 0 && qc.y1 == 0);
		assert(c.nextx > 0.0f);
	}

	fonsDeleteInternal(pg);
	assert(rec.deleteShared == 1);
	return 0;
}

/* ================================================================ */
/* Case 4: capacity limits, create failure and retry                 */
/* ================================================================ */

static int case_capacity_and_create_failure(void)
{
	Recorder rec;
	FONScontext* pg;
	SRef ref;
	FONScontext* sg;
	int i;

	/* Invalid parameters: NULL, no callbacks, no events, userPtr retained. */
	{
		FONSpagedParams bad;
		Recorder probe;
		memset(&probe, 0, sizeof(probe));
		memset(&bad, 0, sizeof(bad));
		assert(fonsCreateInternalPaged(NULL) == NULL);
		bad.width = 7; bad.height = 64; bad.maxPages = 4;
		bad.renderCreatePage = rec_create_page;
		bad.renderUpdatePage = rec_update_page;
		bad.renderDrawPage = rec_draw_page;
		bad.renderDeletePage = rec_delete_page;
		bad.userPtr = &probe;
		assert(fonsCreateInternalPaged(&bad) == NULL);
		assert(probe.createCalls == 0 && probe.deleteShared == 0);
		bad.width = 64; bad.height = 4097;
		assert(fonsCreateInternalPaged(&bad) == NULL);
		bad.height = 64; bad.maxPages = 0;
		assert(fonsCreateInternalPaged(&bad) == NULL);
		bad.maxPages = 65;
		assert(fonsCreateInternalPaged(&bad) == NULL);
		bad.maxPages = 4; bad.renderCreatePage = NULL;
		assert(fonsCreateInternalPaged(&bad) == NULL);
		assert(probe.createCalls == 0 && probe.deleteShared == 0);
	}

	/* First page creation fails: library cleans up and calls renderDelete. */
	{
		Recorder early;
		FONScontext* failed;
		failed = make_paged(&early, 64, 64, 4, FONS_ZERO_TOPLEFT);
		assert(failed);
		fonsDeleteInternal(failed);
		memset(&early, 0, sizeof(early));
		early.w = early.h = 64; early.maxPages = 4;
		early.failCreatesLeft = 1;
		failed = fonsCreateInternalPaged(&(FONSpagedParams){
			64, 64, 4, FONS_ZERO_TOPLEFT, &early,
			rec_create_page, rec_update_page, rec_draw_page,
			rec_delete_page, rec_delete_shared });
		assert(failed == NULL);
		assert(early.createCalls == 1);
		assert(early.deletePageCalls == 0);
		assert(early.deleteShared == 1);
	}

	/* Page cap: dropped images, full pen advance, cached glyphs still placed. */
	pg = make_paged(&rec, 64, 64, 2, FONS_ZERO_TOPLEFT);
	sg = make_single(&ref, 1024, 1024, FONS_ZERO_TOPLEFT);
	assert(pg && sg);
	add_font(pg); add_font(sg);
	fonsSetErrorCallback(pg, rec_error, &rec);
	{
		float pEnd, sEnd;
		float pv[8192], puv[8192];
		int pn;
		pEnd = fonsDrawText(pg, 3, 40, PRESSURE, NULL);
		sEnd = fonsDrawText(sg, 3, 40, PRESSURE, NULL);
		assert(rec.full > 0);
		assert(fonsGetAtlasPageCount(pg) == 2);
		/* Every glyph still advanced by its font metrics. */
		assert(fabsf(pEnd - sEnd) < 1e-5f);
		flatten(&rec, pv, puv, &pn);
		assert(pn > 0 && pn < ref.nverts);
		/* Every emitted quad sits at a position produced by the reference
		 * run (dropped glyphs leave the later quads on the same pen). */
		for (i = 0; i < pn; i += 6) {
			int k, matched = 0;
			for (k = 0; k < ref.nverts; k += 6) {
				if (floats_eq(pv + i*2, ref.verts + k*2, 12)) {
					matched = 1;
					break;
				}
			}
			assert(matched);
		}
	}
	fonsDeleteInternal(pg);
	fonsDeleteInternal(sg);

	/* A glyph larger than an empty page is rejected but layout continues. */
	pg = make_paged(&rec, 64, 64, 4, FONS_ZERO_TOPLEFT);
	assert(pg);
	add_font(pg);
	fonsSetSize(pg, 96.0f);
	fonsSetErrorCallback(pg, rec_error, &rec);
	{
		float x0 = 5.0f;
		float x1 = fonsDrawText(pg, x0, 110, "日", NULL);
		assert(rec.full > 0);
		assert(fonsGetAtlasPageCount(pg) == 1);
		assert(x1 > x0);
		assert(rec.ndraws == 0);
	}
	fonsDeleteInternal(pg);

	/* One failed new page: not counted, no delete, no residency, retry works. */
	pg = make_paged(&rec, 64, 64, 8, FONS_ZERO_TOPLEFT);
	assert(pg);
	add_font(pg);
	fonsSetErrorCallback(pg, rec_error, &rec);
	fonsDrawText(pg, 0, 40, PRESSURE, NULL); /* spans several pages */
	{
		int pages = fonsGetAtlasPageCount(pg);
		int fullBefore = rec.full;
		FONStextIter it;
		FONSquad q;
		assert(pages >= 3);
		/* A new size means none of these glyphs are cached; the existing
		 * pages fill up and every further page creation is made to fail. */
		fonsSetSize(pg, 20.0f);
		rec.failCreatesLeft = 100000;
		fonsDrawText(pg, 0, 80, PRESSURE, NULL);
		assert(rec.full > fullBefore);
		assert(fonsGetAtlasPageCount(pg) == pages);
		/* At least one glyph of the failed run has no residency record. */
		assert(fonsTextIterInit(pg, &it, 0, 80, PRESSURE, NULL));
		while (fonsTextIterNext(pg, &it, &q))
			if (it.page == -1) break;
		assert(it.page == -1);
		/* Retry with the backend healthy: consecutive pages appear. */
		rec.failCreatesLeft = 0;
		{
			int createsBefore = rec.createCalls;
			fonsDrawText(pg, 0, 80, PRESSURE, NULL);
			assert(rec.createCalls > createsBefore);
			assert(fonsGetAtlasPageCount(pg) > pages);
		}
		fonsSetSize(pg, 24.0f);
	}
	{
		int pages = fonsGetAtlasPageCount(pg);
		fonsDeleteInternal(pg);
		assert(rec.deletePageCalls == pages);
		assert(rec.deleteShared == 1);
		for (i = 0; i < pages; i++)
			assert(rec.deleteOrder[i] == i);
	}
	return 0;
}

/* ================================================================ */
/* Case 5: GL2 backend page lifetime through the driver recorder     */
/* ================================================================ */

static int alive_textures(void)
{
	int i, n = 0;
	for (i = 1; i < 128; i++)
		if (spy.textures[i].alive) n++;
	return n;
}

static int case_gl2_page_lifetime(void)
{
	FONScontext* pg;
	FONScontext* legacy;
	FONScontext* small;
	FONStextIter it;
	FONSquad q;
	unsigned int cp0 = 0, cp1 = 0;
	char triple[16];
	int n, i, pages;
	int updatesBefore[128];

	memset(&spy, 0, sizeof(spy));

	/* Invalid parameters fail before any GL call. */
	assert(glfonsCreatePaged(7, 64, 4, FONS_ZERO_TOPLEFT) == NULL);
	assert(glfonsCreatePaged(64, 64, 0, FONS_ZERO_TOPLEFT) == NULL);
	assert(spy.creates == 0);

	pg = glfonsCreatePaged(64, 64, 16, FONS_ZERO_TOPLEFT);
	assert(pg);
	add_font(pg);

	/* Drive several pages. */
	fonsDrawText(pg, 2, 40, PRESSURE, NULL);
	pages = fonsGetAtlasPageCount(pg);
	assert(pages >= 3);
	assert(alive_textures() == pages);
	/* One texture per created page, each uploaded before being drawn. */
	assert(spy.creates == pages);
	for (i = 0; i < spy.draw_count; i++) {
		GLuint tex = spy.draws[i].texture;
		assert(spy.textures[tex].alive);
		assert(spy.textures[tex].updates > 0);
	}

	/* Submission order 0, 1, 0 follows the input order. */
	assert(fonsTextIterInit(pg, &it, 0, 40, PRESSURE, NULL));
	while (fonsTextIterNext(pg, &it, &q)) {
		if (it.page == 0 && cp0 == 0) cp0 = it.codepoint;
		if (it.page == 1 && cp1 == 0) cp1 = it.codepoint;
	}
	assert(cp0 && cp1);
	n = 0;
	n += put_utf8(cp0, triple+n);
	n += put_utf8(cp1, triple+n);
	n += put_utf8(cp0, triple+n);
	triple[n] = '\0';
	{
		int drawsBefore = spy.draw_count;
		for (i = 1; i < 128; i++) updatesBefore[i] = spy.textures[i].updates;
		fonsDrawText(pg, 2, 80, triple, NULL);
		/* Cached glyphs: no new textures, no extra TexSubImage. */
		assert(spy.creates == pages);
		for (i = 1; i < 128; i++)
			assert(spy.textures[i].updates == updatesBefore[i]);
		/* Three draws bound in input order: page 0, page 1, page 0. */
		assert(spy.draw_count - drawsBefore == 3);
		{
			GLuint t0 = spy.draws[drawsBefore].texture;
			GLuint t1 = spy.draws[drawsBefore+1].texture;
			assert(t1 != t0);
			assert(spy.draws[drawsBefore+2].texture == t0);
			/* Both textures already existed: cached run created nothing. */
			assert(spy.textures[t0].alive && spy.textures[t1].alive);
		}
	}

	/* Page creation failure via injected GL fault, then a successful retry. */
	{
		int aliveBefore, pagesBefore;
		small = glfonsCreatePaged(64, 64, 16, FONS_ZERO_TOPLEFT);
		assert(small);
		add_font(small);
		fonsDrawText(small, 0, 40, PRESSURE, NULL); /* spans several pages */
		pagesBefore = fonsGetAtlasPageCount(small);
		assert(pagesBefore >= 3);
		aliveBefore = alive_textures();
		/* Uncached glyphs (new size) exhaust the existing pages; the next
		 * texture creation is injected as a driver failure. */
		fonsSetSize(small, 20.0f);
		spy.fail_create_remaining = 100000;
		fonsDrawText(small, 0, 80, PRESSURE, NULL);
		/* No new texture appeared and the page count is unchanged. */
		assert(alive_textures() == aliveBefore);
		assert(fonsGetAtlasPageCount(small) == pagesBefore);
		spy.fail_create_remaining = 0;
		/* Retry now materializes a fresh, consecutive page. The shared spy
		 * still holds 'pg's textures, so compare deltas. */
		{
			int aliveAtRetry = alive_textures();
			fonsDrawText(small, 0, 80, PRESSURE, NULL);
			assert(fonsGetAtlasPageCount(small) > pagesBefore);
			assert(alive_textures() - aliveAtRetry ==
				   fonsGetAtlasPageCount(small) - pagesBefore);
		}
		glfonsDelete(small);
	}

	/* Legacy single page GL backend keeps working alongside the paged one. */
	legacy = glfonsCreate(128, 128, FONS_ZERO_TOPLEFT);
	assert(legacy);
	add_font(legacy);
	fonsDrawText(legacy, 3, 40, "日本", NULL);
	glfonsDelete(legacy);

	/* Deleting the paged stash removes every texture exactly once and frees
	 * the shared backend after the last texture is gone. */
	{
		int aliveBefore = alive_textures();
		assert(aliveBefore == pages);
		glfonsDelete(pg);
		assert(alive_textures() == 0);
		assert(spy.deletes >= aliveBefore);
	}
	return 0;
}

/* ------------------------------------------------------------------ */

const PagedCase paged_cases[] = {
	{ "layout_without_residency", case_layout_without_residency },
	{ "page_order_and_pixels", case_page_order_and_pixels },
	{ "cache_and_iterators", case_cache_and_iterators },
	{ "capacity_and_create_failure", case_capacity_and_create_failure },
	{ "gl2_page_lifetime", case_gl2_page_lifetime },
};
const size_t paged_case_count = sizeof(paged_cases)/sizeof(paged_cases[0]);
