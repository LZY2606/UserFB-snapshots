/* evenodd_pgm.c - Example for the SFT_EVEN_ODD fill rule.
 *
 * Generates a small geometric TrueType font entirely in memory (no font
 * files are loaded), renders the same glyph with the default winding rule
 * and with the even-odd rule, and writes both grayscale images as binary
 * PGM files:
 *
 *   winding.pgm  - default signed-area / non-zero-winding rendering
 *   evenodd.pgm  - SFT_EVEN_ODD rendering
 *
 * The glyph contains a bowtie (self-intersecting contour), two coincident
 * squares and a plain square, so the two fill rules produce visibly
 * different results.
 *
 * Build: cc -std=c99 -O2 -I. evenodd_pgm.c -L. -lschrift -lm -o evenodd_pgm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schrift.h"

/* ----------------------------------------------------------- TTF builder */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

static void buf_reserve(Buf *b, size_t extra)
{
	if (b->len + extra <= b->cap)
		return;
	size_t cap = b->cap ? b->cap * 2 : 128;
	while (cap < b->len + extra)
		cap *= 2;
	uint8_t *p = (uint8_t *) realloc(b->data, cap);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->data = p;
	b->cap = cap;
}

static void put16(Buf *b, unsigned v)
{
	buf_reserve(b, 2);
	b->data[b->len++] = (uint8_t) (v >> 8);
	b->data[b->len++] = (uint8_t) v;
}

static void put32(Buf *b, unsigned v)
{
	put16(b, v >> 16);
	put16(b, v & 0xFFFF);
}

static void put_bytes(Buf *b, const uint8_t *p, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static unsigned checksum(const Buf *b)
{
	unsigned s = 0;
	size_t i;
	for (i = 0; i < b->len; i += 4) {
		unsigned w = 0;
		size_t k;
		for (k = 0; k < 4; ++k) {
			w <<= 8;
			if (i + k < b->len)
				w |= b->data[i + k];
		}
		s += w;
	}
	return s;
}

struct Pt { int x, y; };

/* Encodes one simple glyph made of axis-aligned polygon contours. */
static Buf simple_glyph(const struct Pt *pts, size_t numPts,
                        const unsigned *endPts, size_t numContours,
                        int xmin, int ymin, int xmax, int ymax)
{
	Buf g = {0};
	size_t i;
	int prev;
	put16(&g, (unsigned) numContours);
	put16(&g, (unsigned) (xmin + 65536));
	put16(&g, (unsigned) (ymin + 65536));
	put16(&g, (unsigned) (xmax + 65536));
	put16(&g, (unsigned) (ymax + 65536));
	for (i = 0; i < numContours; ++i)
		put16(&g, endPts[i]);
	put16(&g, 0); /* instruction length */
	for (i = 0; i < numPts; ++i)
		g.data[g.len++] = 1; /* POINT_IS_ON_CURVE */
	prev = 0;
	for (i = 0; i < numPts; ++i) { put16(&g, (unsigned) (pts[i].x - prev + 65536)); prev = pts[i].x; }
	prev = 0;
	for (i = 0; i < numPts; ++i) { put16(&g, (unsigned) (pts[i].y - prev + 65536)); prev = pts[i].y; }
	while (g.len & 1)
		g.data[g.len++] = 0;
	return g;
}

struct Table { char tag[4]; Buf body; unsigned offset; };

/* Wraps one glyph body into a minimal font and returns the mapped glyph id. */
static Buf make_font(Buf glyfBody, unsigned *glyphId)
{
	struct Table t[7];
	Buf head = {0}, hhea = {0}, maxp = {0}, loca = {0};
	Buf hmtx = {0}, cmap = {0}, out = {0};
	unsigned i, n;

	/* head, indexToLocFormat = 1 (long loca). Fields sit at fixed offsets. */
	buf_reserve(&head, 54);
	memset(head.data, 0, 54);
	head.len = 54;
	head.data[0] = 0x00; head.data[1] = 0x01; head.data[2] = 0x00; head.data[3] = 0x00;
	head.data[18] = 0; head.data[19] = 64; /* unitsPerEm */
	head.data[50] = 0; head.data[51] = 1;  /* indexToLocFormat */

	/* hhea is a fixed 36-byte table; numberOfHMetrics lives at offset 34. */
	buf_reserve(&hhea, 36);
	memset(hhea.data, 0, 36);
	hhea.len = 36;
	hhea.data[0] = 0x00; hhea.data[1] = 0x01; hhea.data[2] = 0x00; hhea.data[3] = 0x00;
	hhea.data[34] = 0; hhea.data[35] = 2;

	put32(&maxp, 0x00010000);
	put16(&maxp, 2);  /* numGlyphs */
	put16(&maxp, 32); /* maxPoints */
	put16(&maxp, 4);  /* maxContours */
	put16(&maxp, 2);  /* maxCompositePoints */

	put32(&loca, 0); /* glyph 0 empty */
	put32(&loca, 0); /* glyph 1 starts at 0 */
	put32(&loca, (unsigned) glyfBody.len);

	put16(&hmtx, 52); put16(&hmtx, 0); /* glyph 0 metrics */
	put16(&hmtx, 52); put16(&hmtx, 0); /* glyph 1 metrics */

	/* Format 6 cmap: U+0041 -> glyph 1. */
	put16(&cmap, 0); put16(&cmap, 1);
	put16(&cmap, 3); put16(&cmap, 1); put32(&cmap, 12);
	put16(&cmap, 6); put16(&cmap, 10 + 2); put16(&cmap, 0);
	put16(&cmap, 65); put16(&cmap, 1);
	put16(&cmap, 1);

	memcpy(t[0].tag, "head", 4); t[0].body = head;
	memcpy(t[1].tag, "hhea", 4); t[1].body = hhea;
	memcpy(t[2].tag, "maxp", 4); t[2].body = maxp;
	memcpy(t[3].tag, "loca", 4); t[3].body = loca;
	memcpy(t[4].tag, "glyf", 4); t[4].body = glyfBody;
	memcpy(t[5].tag, "hmtx", 4); t[5].body = hmtx;
	memcpy(t[6].tag, "cmap", 4); t[6].body = cmap;

	/* The table directory must be sorted by tag (the library looks tables up
	 * with bsearch). Simple insertion sort over seven entries. */
	for (i = 1; i < 7; ++i) {
		struct Table key = t[i];
		int j = (int) i - 1;
		while (j >= 0 && memcmp(t[j].tag, key.tag, 4) > 0) {
			t[j + 1] = t[j];
			--j;
		}
		t[j + 1] = key;
	}

	n = 12 + 16 * 7;
	for (i = 0; i < 7; ++i) {
		t[i].offset = n;
		n += (unsigned) t[i].body.len + (-(int) t[i].body.len & 3);
	}
	buf_reserve(&out, n);
	put32(&out, 0x00010000);
	put16(&out, 7);
	put16(&out, 64);
	put16(&out, 2);
	put16(&out, 16 * 7 - 64);
	for (i = 0; i < 7; ++i) {
		put_bytes(&out, (const uint8_t *) t[i].tag, 4);
		put32(&out, checksum(&t[i].body));
		put32(&out, t[i].offset);
		put32(&out, (unsigned) t[i].body.len);
	}
	for (i = 0; i < 7; ++i) {
		put_bytes(&out, t[i].body.data, t[i].body.len);
		while (out.len & 3)
			out.data[out.len++] = 0;
	}
	/* Patch the head checkSumAdjustment. */
	for (i = 0; i < 7; ++i)
		if (!memcmp(t[i].tag, "head", 4)) {
			unsigned at = t[i].offset + 8;
			unsigned adj = 0xB1B0AFBA - checksum(&out);
			out.data[at]     = (uint8_t) (adj >> 24);
			out.data[at + 1] = (uint8_t) (adj >> 16);
			out.data[at + 2] = (uint8_t) (adj >> 8);
			out.data[at + 3] = (uint8_t) adj;
		}
	/* The glyf entry aliases the caller-owned glyfBody buffer, so only the
	 * locally built table buffers are released. */
	for (i = 0; i < 7; ++i)
		if (memcmp(t[i].tag, "glyf", 4) != 0)
			free(t[i].body.data);

	*glyphId = 1;
	return out;
}

/* ----------------------------------------------------------------- render */

static int write_pgm(const char *path, const uint8_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return -1;
	}
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	fwrite(px, 1, (size_t) w * (size_t) h, f);
	fclose(f);
	printf("wrote %s (%dx%d)\n", path, w, h);
	return 0;
}

static int render_rule(const uint8_t *fontBytes, size_t fontSize,
                       SFT_Glyph glyph, int flags, const char *path)
{
	SFT_Font *font = sft_loadmem(fontBytes, fontSize);
	SFT s;
	SFT_GMetrics m;
	uint8_t *px;
	int rc;
	if (!font) {
		fprintf(stderr, "sft_loadmem failed\n");
		return -1;
	}
	memset(&s, 0, sizeof s);
	s.font = font;
	s.xScale = 8.0;
	s.yScale = 8.0;
	s.flags = flags;
	if (sft_gmetrics(&s, glyph, &m) < 0) {
		fprintf(stderr, "sft_gmetrics failed\n");
		sft_freefont(font);
		return -1;
	}
	px = (uint8_t *) calloc((size_t) m.minWidth * (size_t) m.minHeight, 1);
	if (!px) {
		sft_freefont(font);
		return -1;
	}
	rc = sft_render(&s, glyph,
	                (SFT_Image) { px, m.minWidth, m.minHeight });
	if (rc == 0)
		rc = write_pgm(path, px, m.minWidth, m.minHeight);
	free(px);
	sft_freefont(font);
	return rc;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char wp[512], ep[512];
	/* Three contours in one glyph, each 64 font units:
	 *  - a bowtie (self-intersecting),
	 *  - two coincident unit squares placed side by side with the bowtie. */
	const struct Pt pts[] = {
		/* bowtie around x in [0,64], y in [0,64] */
		{  0,   0}, { 64,  64}, {  0,  64}, { 64,   0},
		/* first square, shifted right */
		{ 96,   0}, {160,   0}, {160,  64}, { 96,  64},
		/* second square, exactly coincident with the first */
		{ 96,   0}, {160,   0}, {160,  64}, { 96,  64}
	};
	const unsigned endPts[] = { 3, 7, 11 };
	Buf body = simple_glyph(pts, sizeof pts / sizeof pts[0],
	                        endPts, 3, 0, 0, 160, 64);
	SFT_Glyph glyph;
	Buf font = make_font(body, &glyph);
	snprintf(wp, sizeof wp, "%s/winding.pgm", dir);
	snprintf(ep, sizeof ep, "%s/evenodd.pgm", dir);
	int rc = 0;
	rc |= render_rule(font.data, font.len, glyph, 0, wp);
	rc |= render_rule(font.data, font.len, glyph, SFT_EVEN_ODD, ep);
	free(body.data);
	free(font.data);
	return rc ? 1 : 0;
}
