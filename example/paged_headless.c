/* Headless multi-page atlas demo.
 *
 * Renders a string that does not fit on one fixed-size page through a CPU
 * recording backend, composites every texture page into one grayscale PGM
 * (P2) image and reports how many pages were needed. No windows, no GL, no
 * dependencies beyond fontstash and the bundled stb_truetype.
 *
 * Usage: paged_headless output.pgm
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"

#define ATLAS_W 64
#define ATLAS_H 64
#define MAX_PAGES 16
#define CANVAS_W 760
#define CANVAS_H 80
#define FONT_PATH "example/DroidSansJapanese.ttf"
static const char SAMPLE[] = "日本語文字漢東京大阪京都北海道青空山川海花月星金木水火土石";

typedef struct {
	unsigned char* pixels;
	int w, h;
} Page;

typedef struct {
	Page pages[MAX_PAGES];
	int npages;
} Backend;

static int backend_create_page(void* uptr, int page, int width, int height)
{
	Backend* b = (Backend*)uptr;
	assert(page >= 0 && page < MAX_PAGES);
	b->pages[page].w = width;
	b->pages[page].h = height;
	b->pages[page].pixels = (unsigned char*)calloc((size_t)width*height, 1);
	if (b->pages[page].pixels == NULL)
		return 0;
	if (page + 1 > b->npages)
		b->npages = page + 1;
	return 1;
}

static void backend_update_page(void* uptr, int page, int* rect, const unsigned char* data)
{
	Backend* b = (Backend*)uptr;
	Page* p = &b->pages[page];
	int y;
	for (y = rect[1]; y < rect[3]; y++)
		memcpy(p->pixels + y*p->w + rect[0],
			   data + y*p->w + rect[0], (size_t)(rect[2]-rect[0]));
}

static void backend_draw_page(void* uptr, int page, const float* verts,
							   const float* tcoords, const unsigned int* colors,
							   int nverts)
{
	/* The compositor below pulls quads from the iterator instead, so the
	 * draw callback only validates that every batch names a live page. */
	Backend* b = (Backend*)uptr;
	(void)verts; (void)tcoords; (void)colors; (void)nverts;
	assert(page >= 0 && page < b->npages && b->pages[page].pixels != NULL);
}

static void backend_delete_page(void* uptr, int page)
{
	Backend* b = (Backend*)uptr;
	free(b->pages[page].pixels);
	b->pages[page].pixels = NULL;
}

int main(int argc, char** argv)
{
	FONSpagedParams params;
	FONScontext* stash;
	FONStextIter iter;
	FONSquad quad;
	Backend backend;
	unsigned char* canvas;
	FILE* out;
	int font, ink = 0, i, xx, yy;

	if (argc != 2) {
		fprintf(stderr, "usage: %s output.pgm\n", argv[0]);
		return 2;
	}

	memset(&backend, 0, sizeof(backend));
	memset(&params, 0, sizeof(params));
	params.width = ATLAS_W;
	params.height = ATLAS_H;
	params.maxPages = MAX_PAGES;
	params.flags = FONS_ZERO_TOPLEFT;
	params.userPtr = &backend;
	params.renderCreatePage = backend_create_page;
	params.renderUpdatePage = backend_update_page;
	params.renderDrawPage = backend_draw_page;
	params.renderDeletePage = backend_delete_page;
	params.renderDelete = NULL;

	stash = fonsCreateInternalPaged(&params);
	if (stash == NULL) {
		fprintf(stderr, "failed to create paged stash\n");
		return 1;
	}

	font = fonsAddFont(stash, "jp", FONT_PATH);
	if (font < 0) {
		fprintf(stderr, "failed to load font %s\n", FONT_PATH);
		return 1;
	}
	fonsSetFont(stash, font);
	fonsSetSize(stash, 24.0f);
	fonsSetColor(stash, 0xffffffff);

	/* Rasterize on demand; the 64x64 pages fill up and new ones are added. */
	fonsDrawText(stash, 2, 48, SAMPLE, NULL);

	printf("paged_headless: glyphs=%d pages=%d (page size %dx%d)\n",
		   (int)strlen(SAMPLE) / 3, fonsGetAtlasPageCount(stash), ATLAS_W, ATLAS_H);

	/* Composite the resident quads of every page onto one canvas. */
	canvas = (unsigned char*)calloc((size_t)CANVAS_W*CANVAS_H, 1);
	assert(canvas != NULL);

	if (!fonsTextIterInit(stash, &iter, 2, 48, SAMPLE, NULL)) {
		fprintf(stderr, "iterator init failed\n");
		return 1;
	}
	while (fonsTextIterNext(stash, &iter, &quad)) {
		int sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, xx, yy;
		Page* p;
		if (iter.page < 0)
			continue; /* not resident or empty outline: nothing to blit */
		p = &backend.pages[iter.page];

		sx0 = (int)(quad.s0 * p->w + 0.5f);
		sy0 = (int)(quad.t0 * p->h + 0.5f);
		sx1 = (int)(quad.s1 * p->w + 0.5f);
		sy1 = (int)(quad.t1 * p->h + 0.5f);
		dx0 = (int)quad.x0;
		dy0 = (int)quad.y0;
		dx1 = (int)quad.x1;
		dy1 = (int)quad.y1;
		assert(sx1-sx0 == dx1-dx0 && sy1-sy0 == dy1-dy0);

		for (yy = 0; yy < dy1-dy0; yy++) {
			int cy = dy0 + yy;
			if (cy < 0 || cy >= CANVAS_H) continue;
			for (xx = 0; xx < dx1-dx0; xx++) {
				int cx = dx0 + xx;
				int srcx = sx0 + xx;
				int srcy = sy0 + yy;
				unsigned char a;
				if (cx < 0 || cx >= CANVAS_W) continue;
				if (srcx < 0 || srcx >= p->w || srcy < 0 || srcy >= p->h)
					continue;
				a = p->pixels[srcy*p->w + srcx];
				if (a > canvas[cy*CANVAS_W + cx])
					canvas[cy*CANVAS_W + cx] = a;
			}
		}
	}

	out = fopen(argv[1], "wb");
	if (out == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	fprintf(out, "P2\n%d %d\n255\n", CANVAS_W, CANVAS_H);
	for (yy = 0; yy < CANVAS_H; yy++) {
		for (xx = 0; xx < CANVAS_W; xx++) {
			int v = canvas[yy*CANVAS_W + xx];
			fprintf(out, "%d ", v);
			if (v > 0) ink++;
		}
		fputc('\n', out);
	}
	fclose(out);
	printf("paged_headless: wrote %s inkPixels=%d\n", argv[1], ink);
	if (ink == 0) {
		fprintf(stderr, "rendered image has no ink\n");
		return 1;
	}

	free(canvas);
	fonsDeleteInternal(stash); /* releases every page exactly once */
	for (i = 0; i < MAX_PAGES; i++)
		assert(backend.pages[i].pixels == NULL);
	return 0;
}
