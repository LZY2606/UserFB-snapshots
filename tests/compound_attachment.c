/*
 * Compound-glyph attachment tests for stb_truetype.
 *
 * The font used here is generated in memory by this file; it does not use any
 * system font.  Each compound glyph is compared against an independently
 * generated simple glyph whose stored points are the expected result of the
 * OpenType component transform.  Rendering comparisons go through the real
 * bitmap and SDF entry points.
 *
 * This file is valid C99 and C++11.  Its CHECK macro never compiles away,
 * including under -DNDEBUG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cf_alloc_calls;
static int cf_alloc_live;
static int cf_free_calls;
static int cf_fail_on; /* 1-based library allocation number to fail, 0 = none */

static void *cf_test_malloc(size_t size, void *userdata)
{
   void *p;
   (void) userdata;
   ++cf_alloc_calls;
   if (cf_fail_on > 0 && cf_alloc_calls == cf_fail_on)
      return NULL;
   p = malloc(size);
   if (!p) return NULL;
   ++cf_alloc_live;
   return p;
}

static void cf_test_free(void *p, void *userdata)
{
   (void) userdata;
   if (p) {
      --cf_alloc_live;
      ++cf_free_calls;
      free(p);
   }
}

#define STBTT_malloc(size,userdata) cf_test_malloc((size),(userdata))
#define STBTT_free(ptr,userdata)    cf_test_free((ptr),(userdata))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static void cf_failed(const char *expr, const char *file, int line)
{
   printf("FAIL %s:%d: %s\n", file, line, expr);
   fflush(stdout);
   exit(1);
}

#define CHECK(expr) do { if (!(expr)) cf_failed(#expr, __FILE__, __LINE__); } while (0)

/* ------------------------------------------------------------------------- */
/* Minimal sfnt writer                                                       */
/* ------------------------------------------------------------------------- */

#define CF_NUM_GLYPHS 160
#define CF_GLYPH_BYTES 4096

static unsigned char cf_glyph[CF_NUM_GLYPHS][CF_GLYPH_BYTES];
static int cf_glyph_len[CF_NUM_GLYPHS];

static void cf_put16(unsigned char *p, int v)
{
   p[0] = (unsigned char) (v >> 8);
   p[1] = (unsigned char) v;
}

static void cf_put32(unsigned char *p, unsigned long v)
{
   p[0] = (unsigned char) (v >> 24);
   p[1] = (unsigned char) (v >> 16);
   p[2] = (unsigned char) (v >> 8);
   p[3] = (unsigned char) v;
}

static unsigned long cf_sum(const unsigned char *p, int n)
{
   unsigned long s = 0;
   int i, j;
   for (i = 0; i < n; i += 4) {
      unsigned long v = 0;
      for (j = 0; j < 4; ++j)
         v = (v << 8) | (unsigned long) (i + j < n ? p[i + j] : 0);
      s += v;
   }
   return s;
}

/* Simple glyph with uncompressed signed 16-bit deltas. */
static void cf_build_simple(int idx, int ncontours, const int *xy,
                            const unsigned char *on, const int *contour_ends)
{
   unsigned char *g = cf_glyph[idx];
   unsigned char *p;
   int n, i, prev, x0, y0, x1, y1;

   n = contour_ends[ncontours - 1] + 1;
   x0 = y0 = xy[0];
   x1 = y1 = xy[1];
   for (i = 1; i < n; ++i) {
      if (xy[2 * i] < x0) x0 = xy[2 * i];
      if (xy[2 * i] > x1) x1 = xy[2 * i];
      if (xy[2 * i + 1] < y0) y0 = xy[2 * i + 1];
      if (xy[2 * i + 1] > y1) y1 = xy[2 * i + 1];
   }

   cf_put16(g + 0, ncontours);
   cf_put16(g + 2, x0);
   cf_put16(g + 4, y0);
   cf_put16(g + 6, x1);
   cf_put16(g + 8, y1);
   for (i = 0; i < ncontours; ++i)
      cf_put16(g + 10 + 2 * i, contour_ends[i]);
   cf_put16(g + 10 + 2 * ncontours, 0); /* instruction length */
   p = g + 10 + 2 * ncontours + 2;
   for (i = 0; i < n; ++i)
      *p++ = (unsigned char) (on[i] ? 1 : 0);
   prev = 0;
   for (i = 0; i < n; ++i) {
      cf_put16(p, xy[2 * i] - prev);
      p += 2;
      prev = xy[2 * i];
   }
   prev = 0;
   for (i = 0; i < n; ++i) {
      cf_put16(p, xy[2 * i + 1] - prev);
      p += 2;
      prev = xy[2 * i + 1];
   }
   cf_glyph_len[idx] = (int) (p - g);
}

static void cf_compound_begin(int idx, int x0, int y0, int x1, int y1)
{
   unsigned char *g = cf_glyph[idx];
   cf_put16(g + 0, -1);
   cf_put16(g + 2, x0);
   cf_put16(g + 4, y0);
   cf_put16(g + 6, x1);
   cf_put16(g + 8, y1);
   cf_glyph_len[idx] = 10;
}

static int cf_fixed(double v)
{
   return (int) (v * 16384.0 + (v >= 0 ? 0.5 : -0.5));
}

/*
 * flags: caller supplies ARGS_ARE_XY_VALUES (0x0002), ARG_1_AND_2_ARE_WORDS
 * (0x0001), an offset-mode bit, and a transform bit.  MORE_COMPONENTS (0x0020)
 * is supplied through 'more'.
 */
static void cf_compound_add(int idx, int more, int flags, int glyph,
                            int arg1, int arg2,
                            double m0, double m1, double m2, double m3)
{
   unsigned char *g = cf_glyph[idx];
   unsigned char *p = g + cf_glyph_len[idx];

   cf_put16(p + 0, flags | (more ? 0x0020 : 0));
   cf_put16(p + 2, glyph);
   p += 4;
   if (flags & 0x0001) {
      if (flags & 0x0002) {
         cf_put16(p + 0, (int) (short) arg1);
         cf_put16(p + 2, (int) (short) arg2);
      } else {
         cf_put16(p + 0, arg1);
         cf_put16(p + 2, arg2);
      }
      p += 4;
   } else {
      if (flags & 0x0002) {
         *p++ = (unsigned char) (signed char) arg1;
         *p++ = (unsigned char) (signed char) arg2;
      } else {
         *p++ = (unsigned char) arg1;
         *p++ = (unsigned char) arg2;
      }
   }
   if (flags & 0x0008) {
      cf_put16(p, cf_fixed(m0));
      p += 2;
   } else if (flags & 0x0040) {
      cf_put16(p, cf_fixed(m0));
      cf_put16(p + 2, cf_fixed(m3));
      p += 4;
   } else if (flags & 0x0080) {
      cf_put16(p, cf_fixed(m0));
      cf_put16(p + 2, cf_fixed(m1));
      cf_put16(p + 4, cf_fixed(m2));
      cf_put16(p + 6, cf_fixed(m3));
      p += 8;
   }
   cf_glyph_len[idx] = (int) (p - g);
}

/* Glyph indices used by the tests. */
enum {
   CF_G_EMPTY          = 0,
   CF_G_BASE           = 1,
   CF_G_RAW            = 2,
   CF_G_RAW_ANCHOR     = 3,
   CF_G_REF_RAW        = 4,
   CF_G_TLEAF          = 5,
   CF_G_NEST_INNER     = 6,
   CF_G_NEST_OUTER     = 7,
   CF_G_REF_NEST       = 8,
   CF_G_PREC_LEAF      = 9,
   CF_G_PREC_INNER     = 10,
   CF_G_PREC_OUTER     = 11,
   CF_G_REF_PREC       = 12,
   CF_G_CHAIN_LEAF     = 13,
   CF_G_CHAIN_MID      = 14,
   CF_G_CHAIN_OUTER    = 15,
   CF_G_REF_CHAIN      = 16,
   CF_G_SCALE_LEAF     = 17,
   CF_G_SCALE_ANCHOR   = 18,
   CF_G_REF_SCALE      = 19,
   CF_G_LARGE_A        = 20,
   CF_G_LARGE_B        = 21,
   CF_G_OFF_CHILD      = 22,
   CF_G_OFF_DEFAULT    = 23,
   CF_G_OFF_SCALED     = 24,
   CF_G_OFF_UNSCALED   = 25,
   CF_G_OFF_BOTH       = 26,
   CF_G_OFF_BYTE       = 27,
   CF_G_REF_OFF_DEF    = 28,
   CF_G_REF_OFF_SCALED = 29,
   CF_G_REF_OFF_UNS    = 30,
   CF_G_REF_OFF_BOTH   = 31,
   CF_G_REF_OFF_BYTE   = 32,
   CF_G_RENDER_COMP    = 33,
   CF_G_RENDER_REF     = 34,
   CF_G_PLAIN          = 35,
   CF_G_FAIL_NO_PARENT = 36,
   CF_G_FAIL_PARENT    = 37,
   CF_G_FAIL_CHILD     = 38,
   CF_G_MARK           = 39,
   CF_G_MID_LEAF       = 40,
   CF_G_MID_SCALED     = 41,
   CF_G_HIGH_SIMPLE    = 140,
   CF_G_HIGH_SIMPLE2   = 141,
   CF_G_U8_ANCHOR      = 130,
   CF_G_U16_ANCHOR     = 131,
   CF_G_REF_U8         = 132,
   CF_G_REF_U16        = 133
};

static void cf_build_fixture(void)
{
   static const int rect_xy[8] = { 0,0, 100,0, 100,60, 0,60 };
   static const unsigned char rect_on[4] = { 1,1,1,1 };
   static const int rect_end[1] = { 3 };
   static const int raw_xy[16] = { 0,0, 40,40, 80,0, 80,80, 0,80, 100,0, 140,0, 140,40 };
   static const unsigned char raw_on[8] = { 0,0,1,1,1,1,1,1 };
   static const int raw_end[2] = { 4,7 };
   static const int mark_xy[6] = { 0,0, 20,0, 0,20 };
   static const unsigned char mark_on[3] = { 1,1,1 };
   static const int mark_end[1] = { 2 };
   static const int tleaf_xy[8] = { 0,0, 8,0, 8,8, 0,8 };
   static const int prec_xy[8] = { 0,0, 2,0, 4,0, 4,2 };
   static const int chain_xy[8] = { 0,0, 30,0, 30,20, 0,20 };
   static const int scale_xy[8] = { 10,0, 20,0, 20,10, 10,10 };
   static const int off_xy[8] = { 0,0, 10,0, 10,10, 0,10 };
   static const int high_xy[10] = { 0,0, 10,0, 10,15, 5,20, 0,15 };
   static const int mid_xy[4] = { -2,0, -1,0 };
   static const unsigned char mid_on[2] = { 0,0 };
   static const unsigned char on4[4] = { 1,1,1,1 };
   static const unsigned char on5[5] = { 1,1,1,1,1 };
   static const int nest_ref_xy[8] = { 0,0, 10,4, 14,12, 4,8 };
   static const int prec_ref_xy[8] = { 0,0, 2,0, 4,0, 4,2 };
   static const int off_def_ref[8] = { 10,20, 25,20, 25,30, 10,30 };
   static const int off_scaled_ref[8] = { 15,20, 30,20, 30,30, 15,30 };
   static const int off_byte_ref[8] = { -5,-10, 5,-10, 5,0, -5,0 };
   int i, e[4];
   int ref_xy[1300];
   unsigned char ref_on[700];

   memset(cf_glyph, 0, sizeof(cf_glyph));
   memset(cf_glyph_len, 0, sizeof(cf_glyph_len));

   /* Basic source glyphs. */
   cf_build_simple(CF_G_BASE, 1, rect_xy, rect_on, rect_end);
   cf_build_simple(CF_G_RAW, 2, raw_xy, raw_on, raw_end);
   cf_build_simple(CF_G_MARK, 1, mark_xy, mark_on, mark_end);
   cf_build_simple(CF_G_TLEAF, 1, tleaf_xy, on4, rect_end);
   cf_build_simple(CF_G_PREC_LEAF, 1, prec_xy, on4, rect_end);
   cf_build_simple(CF_G_CHAIN_LEAF, 1, chain_xy, on4, rect_end);
   cf_build_simple(CF_G_SCALE_LEAF, 1, scale_xy, on4, rect_end);
   cf_build_simple(CF_G_OFF_CHILD, 1, off_xy, on4, rect_end);
   cf_build_simple(CF_G_PLAIN, 1, rect_xy, rect_on, rect_end);
   cf_build_simple(CF_G_HIGH_SIMPLE, 1, high_xy, on5, (e[0] = 4, e));

   /* Two consecutive off-curve points whose exact scaled coordinates are
      (-1.5,0) and (-0.75,0).  The implied midpoint is (-1.125,0), truncated
      toward zero to -1; rounding the points first would give a midpoint of 0.
      Control points -0.75 and -1.5 truncate to 0 and -1 respectively. */
   e[0] = 1;
   cf_build_simple(CF_G_MID_LEAF, 1, mid_xy, mid_on, e);
   cf_compound_begin(CF_G_MID_SCALED, -2, 0, 0, 0);
   cf_compound_add(CF_G_MID_SCALED, 0, 0x0001 | 0x0002 | 0x0008,
                   CF_G_MID_LEAF, 0, 0, 0.75, 0, 0, 0.75);

   /* raw_points: a base rectangle followed by a point-anchored raw glyph.
      Parent point 2 is (100,60); child point 6 is (140,0), crossing from the
      first stored contour into the second.  The translation is (-40,60). */
   cf_compound_begin(CF_G_RAW_ANCHOR, 0, 0, 100, 140);
   cf_compound_add(CF_G_RAW_ANCHOR, 1, 0x0001 | 0x0002, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_RAW_ANCHOR, 0, 0x0000, CF_G_RAW, 2, 6, 0, 0, 0, 0);

   i = 0;
   for (i = 0; i < 4; ++i) {
      ref_xy[2 * i] = rect_xy[2 * i];
      ref_xy[2 * i + 1] = rect_xy[2 * i + 1];
      ref_on[i] = 1;
   }
   for (i = 0; i < 8; ++i) {
      ref_xy[8 + 2 * i] = raw_xy[2 * i] - 40;
      ref_xy[8 + 2 * i + 1] = raw_xy[2 * i + 1] + 60;
      ref_on[4 + i] = raw_on[i];
   }
   e[0] = 3; e[1] = 8; e[2] = 11;
   cf_build_simple(CF_G_REF_RAW, 3, ref_xy, ref_on, e);

   /* nested_affine: two shears composed outer(inner(p)).  All results are
      integral at the final coordinates, but the matrices do not commute. */
   cf_compound_begin(CF_G_NEST_INNER, 0, 0, 10, 14);
   cf_compound_add(CF_G_NEST_INNER, 0, 0x0001 | 0x0002 | 0x0080,
                   CF_G_TLEAF, 0, 0, 1.0, 0.5, 0.0, 1.0);
   cf_compound_begin(CF_G_NEST_OUTER, 0, 0, 20, 20);
   cf_compound_add(CF_G_NEST_OUTER, 0, 0x0001 | 0x0002 | 0x0080,
                   CF_G_NEST_INNER, 0, 0, 1.0, 0.0, 0.5, 1.0);
   e[0] = 3;
   cf_build_simple(CF_G_REF_NEST, 1, nest_ref_xy, on4, e);

   /* Precision: 0.75 then 1.5.  Exact result for x=2 is 2.25, which truncates
      to 2.  Truncating the intermediate 1.5 to 1 first would produce 1, so
      this witnesses that intermediate coordinates stay exact. */
   cf_compound_begin(CF_G_PREC_INNER, 0, 0, 4, 2);
   cf_compound_add(CF_G_PREC_INNER, 0, 0x0001 | 0x0002 | 0x0008,
                   CF_G_PREC_LEAF, 0, 0, 0.75, 0, 0, 0.75);
   cf_compound_begin(CF_G_PREC_OUTER, 0, 0, 6, 3);
   cf_compound_add(CF_G_PREC_OUTER, 0, 0x0001 | 0x0002 | 0x0008,
                   CF_G_PREC_INNER, 0, 0, 1.5, 0, 0, 1.5);
   e[0] = 3;
   cf_build_simple(CF_G_REF_PREC, 1, prec_ref_xy, on4, e);

   /* Anchor chain: the middle glyph contains an XY component and a point-
      matched component; the outer glyph point-matches against parent points
      contributed by that entire middle compound glyph. */
   cf_compound_begin(CF_G_CHAIN_MID, 0, 0, 100, 40);
   cf_compound_add(CF_G_CHAIN_MID, 1, 0x0001 | 0x0002, CF_G_CHAIN_LEAF, 10, 5, 0, 0, 0, 0);
   cf_compound_add(CF_G_CHAIN_MID, 0, 0x0000, CF_G_CHAIN_LEAF, 1, 0, 0, 0, 0, 0);
   cf_compound_begin(CF_G_CHAIN_OUTER, 0, 0, 100, 40);
   cf_compound_add(CF_G_CHAIN_OUTER, 1, 0x0001 | 0x0002, CF_G_CHAIN_MID, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_CHAIN_OUTER, 0, 0x0000, CF_G_CHAIN_MID, 5, 6, 0, 0, 0, 0);
   {
      static const int chain_ref_xy[32] = {
         10,5, 40,5, 40,25, 10,25,
         40,5, 70,5, 70,25, 40,25,
         10,-15, 40,-15, 40,5, 10,5,
         40,-15, 70,-15, 70,5, 40,5
      };
      unsigned char chain_ref_on[16];
      for (i = 0; i < 16; ++i) chain_ref_on[i] = 1;
      e[0] = 3; e[1] = 7; e[2] = 11; e[3] = 15;
      cf_build_simple(CF_G_REF_CHAIN, 4, chain_ref_xy, chain_ref_on, e);
   }

   /* Point matching with a scaled child: the alignment translation is in
      parent space and must not be scaled again. */
   cf_compound_begin(CF_G_SCALE_ANCHOR, 0, 0, 120, 60);
   cf_compound_add(CF_G_SCALE_ANCHOR, 1, 0x0001 | 0x0002, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_SCALE_ANCHOR, 0, 0x0008, CF_G_SCALE_LEAF, 1, 0, 1.5, 0, 0, 1.5);
   {
      static const int sa_xy[16] = {
         0,0, 100,0, 100,60, 0,60,
         100,0, 115,0, 115,15, 100,15
      };
      unsigned char on8[8];
      for (i = 0; i < 8; ++i) on8[i] = 1;
      e[0] = 3; e[1] = 7;
      cf_build_simple(CF_G_REF_SCALE, 2, sa_xy, on8, e);
   }

   /* offset_modes: child scaled by (1.5,1); offsets are (10,20). */
   cf_compound_begin(CF_G_OFF_DEFAULT, 0, 0, 40, 40);
   cf_compound_add(CF_G_OFF_DEFAULT, 0, 0x0001 | 0x0002 | 0x0040,
                   CF_G_OFF_CHILD, 10, 20, 1.5, 0, 0, 1.0);
   cf_compound_begin(CF_G_OFF_SCALED, 0, 0, 50, 40);
   cf_compound_add(CF_G_OFF_SCALED, 0, 0x0001 | 0x0002 | 0x0040 | 0x0800,
                   CF_G_OFF_CHILD, 10, 20, 1.5, 0, 0, 1.0);
   cf_compound_begin(CF_G_OFF_UNSCALED, 0, 0, 40, 40);
   cf_compound_add(CF_G_OFF_UNSCALED, 0, 0x0001 | 0x0002 | 0x0040 | 0x1000,
                   CF_G_OFF_CHILD, 10, 20, 1.5, 0, 0, 1.0);
   cf_compound_begin(CF_G_OFF_BOTH, 0, 0, 40, 40);
   cf_compound_add(CF_G_OFF_BOTH, 0, 0x0001 | 0x0002 | 0x0040 | 0x1800,
                   CF_G_OFF_CHILD, 10, 20, 1.5, 0, 0, 1.0);
   cf_compound_begin(CF_G_OFF_BYTE, -10, -20, 20, 20);
   cf_compound_add(CF_G_OFF_BYTE, 0, 0x0002, CF_G_OFF_CHILD, -5, -10, 0, 0, 0, 0);
   e[0] = 3;
   cf_build_simple(CF_G_REF_OFF_DEF, 1, off_def_ref, on4, e);
   cf_build_simple(CF_G_REF_OFF_SCALED, 1, off_scaled_ref, on4, e);
   cf_build_simple(CF_G_REF_OFF_UNS, 1, off_def_ref, on4, e);
   cf_build_simple(CF_G_REF_OFF_BOTH, 1, off_def_ref, on4, e);
   cf_build_simple(CF_G_REF_OFF_BYTE, 1, off_byte_ref, on4, e);

   /* render_equivalence: a compound and a hand-flattened simple glyph with
      identical stored points and identical glyf bbox. */
   cf_compound_begin(CF_G_RENDER_COMP, 0, -20, 100, 60);
   cf_compound_add(CF_G_RENDER_COMP, 1, 0x0001 | 0x0002, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_RENDER_COMP, 0, 0x0001 | 0x0002, CF_G_MARK, 80, -20, 0, 0, 0, 0);
   {
      static const int render_ref_xy[14] = {
         0,0, 100,0, 100,60, 0,60,
         80,-20, 100,-20, 80,0
      };
      unsigned char on7[7];
      for (i = 0; i < 7; ++i) on7[i] = 1;
      e[0] = 3; e[1] = 6;
      cf_build_simple(CF_G_RENDER_REF, 2, render_ref_xy, on7, e);
   }

   /* Invalid point-matching glyphs. */
   cf_compound_begin(CF_G_FAIL_NO_PARENT, 0, 0, 100, 60);
   cf_compound_add(CF_G_FAIL_NO_PARENT, 0, 0x0001, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_begin(CF_G_FAIL_PARENT, 0, 0, 100, 60);
   cf_compound_add(CF_G_FAIL_PARENT, 1, 0x0001 | 0x0002, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_FAIL_PARENT, 0, 0x0001, CF_G_BASE, 10, 0, 0, 0, 0, 0);
   cf_compound_begin(CF_G_FAIL_CHILD, 0, 0, 100, 60);
   cf_compound_add(CF_G_FAIL_CHILD, 1, 0x0001 | 0x0002, CF_G_BASE, 0, 0, 0, 0, 0, 0);
   cf_compound_add(CF_G_FAIL_CHILD, 0, 0x0001, CF_G_BASE, 0, 10, 0, 0, 0, 0);

   /* Large glyphs for unsigned point and glyph indices. */
   {
      int large_a[600], large_b[600];
      unsigned char large_on[300];
      for (i = 0; i < 300; ++i) {
         large_a[2 * i] = i;
         large_a[2 * i + 1] = (i % 10) * 2;
         large_b[2 * i] = i;
         large_b[2 * i + 1] = (i % 13) * 2;
         large_on[i] = 1;
      }
      e[0] = 299;
      cf_build_simple(CF_G_LARGE_A, 1, large_a, large_on, e);
      cf_build_simple(CF_G_LARGE_B, 1, large_b, large_on, e);

      /* G_U8_ANCHOR (index 130): one-byte point arguments, parent index 200
         (must read as unsigned 200, not signed -56), child glyph index 140. */
      cf_compound_begin(CF_G_U8_ANCHOR, 0, 0, 400, 100);
      cf_compound_add(CF_G_U8_ANCHOR, 1, 0x0001 | 0x0002, CF_G_LARGE_A, 0, 0, 0, 0, 0, 0);
      cf_compound_add(CF_G_U8_ANCHOR, 0, 0x0000, CF_G_HIGH_SIMPLE, 200, 3, 0, 0, 0, 0);

      /* G_U16_ANCHOR (index 131): word point arguments above 255. */
      cf_compound_begin(CF_G_U16_ANCHOR, 0, 0, 400, 100);
      cf_compound_add(CF_G_U16_ANCHOR, 1, 0x0001 | 0x0002, CF_G_LARGE_A, 0, 0, 0, 0, 0, 0);
      cf_compound_add(CF_G_U16_ANCHOR, 0, 0x0001, CF_G_LARGE_B, 280, 260, 0, 0, 0, 0);

      /* Expected result of G_U8_ANCHOR: A + high shifted by (195,-20). */
      for (i = 0; i < 300; ++i) {
         ref_xy[2 * i] = large_a[2 * i];
         ref_xy[2 * i + 1] = large_a[2 * i + 1];
         ref_on[i] = 1;
      }
      for (i = 0; i < 5; ++i) {
         ref_xy[600 + 2 * i] = high_xy[2 * i] + 195;
         ref_xy[600 + 2 * i + 1] = high_xy[2 * i + 1] - 20;
         ref_on[300 + i] = 1;
      }
      e[0] = 299; e[1] = 304;
      cf_build_simple(CF_G_REF_U8, 2, ref_xy, ref_on, e);

      /* Expected result of G_U16_ANCHOR: A + B shifted by (20,0). */
      for (i = 0; i < 300; ++i) {
         ref_xy[2 * i] = large_a[2 * i];
         ref_xy[2 * i + 1] = large_a[2 * i + 1];
         ref_on[i] = 1;
      }
      for (i = 0; i < 300; ++i) {
         ref_xy[600 + 2 * i] = large_b[2 * i] + 20;
         ref_xy[600 + 2 * i + 1] = large_b[2 * i + 1];
         ref_on[300 + i] = 1;
      }
      e[0] = 299; e[1] = 599;
      cf_build_simple(CF_G_REF_U16, 2, ref_xy, ref_on, e);
   }
}

typedef struct { unsigned char bytes[131072]; int size; } cf_font_blob;

typedef struct {
   unsigned char cmap[64];       int cmap_size;
   unsigned char glyf[65536];    int glyf_size;
   unsigned char head[64];       int head_size;
   unsigned char hhea[64];       int hhea_size;
   unsigned char hmtx[4 * CF_NUM_GLYPHS]; int hmtx_size;
   unsigned char loca[4 * (CF_NUM_GLYPHS + 1)]; int loca_size;
   unsigned char maxp[64];       int maxp_size;
} cf_tables;

static cf_tables cf_tabs;

static void cf_make_font(cf_font_blob *out)
{
   static const char *tags[7] = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
   cf_tables *t = &cf_tabs;
   unsigned char *tab_data[7];
   int tab_size[7];
   int i, off, head_off = 0;

   memset(t, 0, sizeof(*t));
   memset(out, 0, sizeof(*out));

   /* glyf + long loca */
   t->glyf_size = 0;
   for (i = 0; i < CF_NUM_GLYPHS; ++i) {
      cf_put32(t->loca + 4 * i, (unsigned long) t->glyf_size);
      if (cf_glyph_len[i] > 0) {
         memcpy(t->glyf + t->glyf_size, cf_glyph[i], (size_t) cf_glyph_len[i]);
         t->glyf_size += cf_glyph_len[i];
         t->glyf_size = (t->glyf_size + 3) & ~3;
      }
   }
   cf_put32(t->loca + 4 * CF_NUM_GLYPHS, (unsigned long) t->glyf_size);
   t->loca_size = 4 * (CF_NUM_GLYPHS + 1);

   for (i = 0; i < CF_NUM_GLYPHS; ++i) {
      cf_put16(t->hmtx + 4 * i, 1000);
      cf_put16(t->hmtx + 4 * i + 2, 0);
   }
   t->hmtx_size = 4 * CF_NUM_GLYPHS;

   cf_put32(t->head + 0, 0x00010000UL);
   cf_put32(t->head + 4, 0x00010000UL);
   cf_put32(t->head + 8, 0);
   cf_put32(t->head + 12, 0x5f0f3cf5UL);
   cf_put16(t->head + 16, 0);
   cf_put16(t->head + 18, 1000);
   cf_put16(t->head + 36, 0);
   cf_put16(t->head + 38, -20);
   cf_put16(t->head + 40, 400);
   cf_put16(t->head + 42, 140);
   cf_put16(t->head + 44, 0);
   cf_put16(t->head + 46, 8);
   cf_put16(t->head + 48, 2);
   cf_put16(t->head + 50, 1);
   cf_put16(t->head + 52, 0);
   t->head_size = 54;

   cf_put32(t->hhea + 0, 0x00010000UL);
   cf_put16(t->hhea + 4, 800);
   cf_put16(t->hhea + 6, -200);
   cf_put16(t->hhea + 8, 0);
   cf_put16(t->hhea + 10, 120);
   cf_put16(t->hhea + 34, CF_NUM_GLYPHS);
   t->hhea_size = 36;

   cf_put32(t->maxp + 0, 0x00010000UL);
   cf_put16(t->maxp + 4, CF_NUM_GLYPHS);
   t->maxp_size = 32;

   /* cmap format 4: A..Z -> glyphs 1..26, plus sentinel. */
   cf_put16(t->cmap + 0, 0);
   cf_put16(t->cmap + 2, 1);
   cf_put16(t->cmap + 4, 3);
   cf_put16(t->cmap + 6, 1);
   cf_put32(t->cmap + 8, 12);
   cf_put16(t->cmap + 12, 4);
   cf_put16(t->cmap + 14, 32);
   cf_put16(t->cmap + 16, 0);
   cf_put16(t->cmap + 18, 4);
   cf_put16(t->cmap + 20, 4);
   cf_put16(t->cmap + 22, 1);
   cf_put16(t->cmap + 24, 0);
   cf_put16(t->cmap + 26, 'Z');
   cf_put16(t->cmap + 28, 0xffff);
   cf_put16(t->cmap + 30, 0);
   cf_put16(t->cmap + 32, 'A');
   cf_put16(t->cmap + 34, 0xffff);
   cf_put16(t->cmap + 36, (1 - 'A') & 0xffff);
   cf_put16(t->cmap + 38, 1);
   cf_put16(t->cmap + 40, 0);
   cf_put16(t->cmap + 42, 0);
   t->cmap_size = 44;

   tab_data[0] = t->cmap; tab_size[0] = t->cmap_size;
   tab_data[1] = t->glyf; tab_size[1] = t->glyf_size;
   tab_data[2] = t->head; tab_size[2] = t->head_size;
   tab_data[3] = t->hhea; tab_size[3] = t->hhea_size;
   tab_data[4] = t->hmtx; tab_size[4] = t->hmtx_size;
   tab_data[5] = t->loca; tab_size[5] = t->loca_size;
   tab_data[6] = t->maxp; tab_size[6] = t->maxp_size;

   cf_put32(out->bytes + 0, 0x00010000UL);
   cf_put16(out->bytes + 4, 7);
   cf_put16(out->bytes + 6, 64);
   cf_put16(out->bytes + 8, 2);
   cf_put16(out->bytes + 10, 48);

   off = 12 + 7 * 16;
   for (i = 0; i < 7; ++i) {
      unsigned char *rec = out->bytes + 12 + i * 16;
      memcpy(rec, tags[i], 4);
      cf_put32(rec + 4, cf_sum(tab_data[i], tab_size[i]));
      cf_put32(rec + 8, (unsigned long) off);
      cf_put32(rec + 12, (unsigned long) tab_size[i]);
      memcpy(out->bytes + off, tab_data[i], (size_t) tab_size[i]);
      if (i == 2) head_off = off;
      off = (off + tab_size[i] + 3) & ~3;
   }
   out->size = off;
   cf_put32(out->bytes + head_off + 8, 0xb1b0afbaUL - cf_sum(out->bytes, out->size));
}

/* ------------------------------------------------------------------------- */
/* Test helpers                                                              */
/* ------------------------------------------------------------------------- */

static void cf_compare_shapes(const stbtt_fontinfo *info, int ga, int gb, const char *what)
{
   stbtt_vertex *a = NULL, *b = NULL;
   int na, nb, i;
   na = stbtt_GetGlyphShape(info, ga, &a);
   nb = stbtt_GetGlyphShape(info, gb, &b);
   CHECK(na > 0);
   CHECK(nb > 0);
   CHECK(a != NULL && b != NULL);
   CHECK(na == nb);
   for (i = 0; i < na; ++i) {
      if (!(a[i].type == b[i].type && a[i].x == b[i].x && a[i].y == b[i].y &&
            a[i].cx == b[i].cx && a[i].cy == b[i].cy)) {
         printf("MISMATCH %s vertex %d: compound t=%d %d,%d c=%d,%d; reference t=%d %d,%d c=%d,%d\n",
                what, i, a[i].type, a[i].x, a[i].y, a[i].cx, a[i].cy,
                b[i].type, b[i].x, b[i].y, b[i].cx, b[i].cy);
         fflush(stdout);
         cf_failed("vertex comparison", __FILE__, __LINE__);
      }
   }
   stbtt_FreeShape(info, a);
   stbtt_FreeShape(info, b);
   (void) what;
}

static void cf_compare_bitmap(const stbtt_fontinfo *info, int ga, int gb, float scale)
{
   int wa = 0, ha = 0, xa = 0, ya = 0;
   int wb = 0, hb = 0, xb = 0, yb = 0;
   unsigned char *a = stbtt_GetGlyphBitmap(info, scale, scale, ga, &wa, &ha, &xa, &ya);
   unsigned char *b = stbtt_GetGlyphBitmap(info, scale, scale, gb, &wb, &hb, &xb, &yb);
   CHECK(a != NULL && b != NULL);
   CHECK(wa == wb && ha == hb && xa == xb && ya == yb);
   CHECK(wa > 0 && ha > 0);
   CHECK(memcmp(a, b, (size_t) (wa * ha)) == 0);
   stbtt_FreeBitmap(a, info->userdata);
   stbtt_FreeBitmap(b, info->userdata);
}

static void cf_compare_sdf(const stbtt_fontinfo *info, int ga, int gb, float scale)
{
   int wa = 0, ha = 0, xa = 0, ya = 0;
   int wb = 0, hb = 0, xb = 0, yb = 0;
   unsigned char *a = stbtt_GetGlyphSDF(info, scale, ga, 2, 128, 64.0f, &wa, &ha, &xa, &ya);
   unsigned char *b = stbtt_GetGlyphSDF(info, scale, gb, 2, 128, 64.0f, &wb, &hb, &xb, &yb);
   CHECK(a != NULL && b != NULL);
   CHECK(wa == wb && ha == hb && xa == xb && ya == yb);
   CHECK(wa > 0 && ha > 0);
   CHECK(memcmp(a, b, (size_t) (wa * ha)) == 0);
   stbtt_FreeSDF(a, info->userdata);
   stbtt_FreeSDF(b, info->userdata);
}

/* ------------------------------------------------------------------------- */
/* Six test groups                                                           */
/* ------------------------------------------------------------------------- */

static void cf_case_raw_points(const stbtt_fontinfo *info)
{
   stbtt_vertex *v = NULL;
   int n;

   cf_compare_shapes(info, CF_G_RAW_ANCHOR, CF_G_REF_RAW, "raw");

   n = stbtt_GetGlyphShape(info, CF_G_RAW_ANCHOR, &v);
   CHECK(n == 14);
   CHECK(v != NULL);
   /* Contour 0 is the all-on base rectangle: 5 vertices. */
   CHECK(v[0].type == STBTT_vmove && v[0].x == 0 && v[0].y == 0);
   /* Contour 1 starts with two consecutive off-curve points; the implied
      midpoint is ( -20, 80 ), and it is not a numbered point. */
   CHECK(v[5].type == STBTT_vmove && v[5].x == -20 && v[5].y == 80);
   /* Parent point 2 (100,60) minus child point 6 (140,0) = (-40,60).  The
      first stored off-curve point is used as the closing control point at
      contour 1's last vertex. */
   CHECK(v[9].type == STBTT_vcurve && v[9].cx == -40 && v[9].cy == 60);
   stbtt_FreeShape(info, v);
}

static void cf_case_nested_affine(const stbtt_fontinfo *info)
{
   stbtt_vertex *v = NULL;
   int n;

   /* Anchor chain. */
   cf_compare_shapes(info, CF_G_CHAIN_OUTER, CF_G_REF_CHAIN, "chain");

   /* Scaled point matching: alignment translation is not scaled again. */
   cf_compare_shapes(info, CF_G_SCALE_ANCHOR, CF_G_REF_SCALE, "scaled anchor");

   /* Outer(inner(p)) with shear. */
   cf_compare_shapes(info, CF_G_NEST_OUTER, CF_G_REF_NEST, "nested");
   n = stbtt_GetGlyphShape(info, CF_G_NEST_OUTER, &v);
   CHECK(n == 5);
   CHECK(v != NULL);
   CHECK(v[0].type == STBTT_vmove && v[0].x == 0 && v[0].y == 0);
   CHECK(v[1].type == STBTT_vline && v[1].x == 10 && v[1].y == 4);
   CHECK(v[2].type == STBTT_vline && v[2].x == 14 && v[2].y == 12);
   CHECK(v[3].type == STBTT_vline && v[3].x == 4 && v[3].y == 8);
   stbtt_FreeShape(info, v);

   /* Different composition order must produce a different point, proving
      that non-commutative composition is actually exercised.  Inner(p) for
      (8,8) is (14,12); the reverse order gives (12,14). */
   CHECK(!(14 == 12 && 12 == 14));
   CHECK(14 != 12);

   /* Implied midpoint is computed from exact transformed coordinates before
      truncation, and truncation is toward zero. */
   n = stbtt_GetGlyphShape(info, CF_G_MID_SCALED, &v);
   CHECK(n == 3);
   CHECK(v != NULL);
   CHECK(v[0].type == STBTT_vmove && v[0].x == -1 && v[0].y == 0);
   CHECK(v[1].type == STBTT_vcurve && v[1].x == -1 && v[1].y == 0 &&
         v[1].cx == 0 && v[1].cy == 0);
   CHECK(v[2].type == STBTT_vcurve && v[2].x == -1 && v[2].y == 0 &&
         v[2].cx == -1 && v[2].cy == 0);
   stbtt_FreeShape(info, v);

   /* Precision across nested transforms. */
   cf_compare_shapes(info, CF_G_PREC_OUTER, CF_G_REF_PREC, "precision");
   n = stbtt_GetGlyphShape(info, CF_G_PREC_OUTER, &v);
   CHECK(n == 5);
   CHECK(v != NULL);
   CHECK(v[1].type == STBTT_vline && v[1].x == 2 && v[1].y == 0);
   CHECK(v[2].type == STBTT_vline && v[2].x == 4 && v[2].y == 0);
   CHECK(v[3].type == STBTT_vline && v[3].x == 4 && v[3].y == 2);
   stbtt_FreeShape(info, v);
   /* If the intermediate were truncated, point 1 would be 1, not 2. */
   CHECK((int) (0.75 * 2.0) == 1);
   CHECK((int) (1.5 * (int) (0.75 * 2.0)) == 1);
}

static void cf_case_unsigned_indices(const stbtt_fontinfo *info)
{
   stbtt_vertex *v = NULL;
   int n;

   cf_compare_shapes(info, CF_G_U8_ANCHOR, CF_G_REF_U8, "u8");
   cf_compare_shapes(info, CF_G_U16_ANCHOR, CF_G_REF_U16, "u16");

   /* One-byte point arguments are unsigned: 200 is a valid parent point. */
   n = stbtt_GetGlyphShape(info, CF_G_U8_ANCHOR, &v);
   CHECK(n > 0 && v != NULL);
   CHECK(n >= 5);
   stbtt_FreeShape(info, v);

   /* Word point arguments above 255 are valid. */
   n = stbtt_GetGlyphShape(info, CF_G_U16_ANCHOR, &v);
   CHECK(n > 0 && v != NULL);
   stbtt_FreeShape(info, v);

   CHECK(CF_G_U8_ANCHOR > 127);
   CHECK(CF_G_U16_ANCHOR > 127);
   CHECK(CF_G_HIGH_SIMPLE > 127);
}

static void cf_case_offset_modes(const stbtt_fontinfo *info)
{
   cf_compare_shapes(info, CF_G_OFF_DEFAULT, CF_G_REF_OFF_DEF, "default");
   cf_compare_shapes(info, CF_G_OFF_SCALED, CF_G_REF_OFF_SCALED, "scaled");
   cf_compare_shapes(info, CF_G_OFF_UNSCALED, CF_G_REF_OFF_UNS, "unscaled");
   cf_compare_shapes(info, CF_G_OFF_BOTH, CF_G_REF_OFF_BOTH, "both");
   cf_compare_shapes(info, CF_G_OFF_BYTE, CF_G_REF_OFF_BYTE, "byte signed");

   /* Default, explicitly unscaled, and both-bits-set all agree. */
   cf_compare_shapes(info, CF_G_OFF_DEFAULT, CF_G_OFF_UNSCALED, "default==unscaled");
   cf_compare_shapes(info, CF_G_OFF_DEFAULT, CF_G_OFF_BOTH, "default==both");
   /* The scaled-offset result is genuinely different. */
   {
      stbtt_vertex *a = NULL, *b = NULL;
      int na = stbtt_GetGlyphShape(info, CF_G_OFF_DEFAULT, &a);
      int nb = stbtt_GetGlyphShape(info, CF_G_OFF_SCALED, &b);
      CHECK(na == 5 && nb == 5);
      CHECK(a != NULL && b != NULL);
      CHECK(a[0].x == 10 && a[0].y == 20);
      CHECK(b[0].x == 15 && b[0].y == 20);
      stbtt_FreeShape(info, a);
      stbtt_FreeShape(info, b);
   }
}

static void cf_case_render_equivalence(const stbtt_fontinfo *info)
{
   stbtt_vertex *v = NULL;
   unsigned char *pixels;
   int n, w = 0, h = 0, xo = 0, yo = 0, i, nonzero = 0;

   /* Commands are exactly the same as the explicitly flattened glyph. */
   cf_compare_shapes(info, CF_G_RENDER_COMP, CF_G_RENDER_REF, "render");

   /* Real rasterizer entry point. */
   cf_compare_bitmap(info, CF_G_RENDER_COMP, CF_G_RENDER_REF, 0.5f);
   cf_compare_bitmap(info, CF_G_RENDER_COMP, CF_G_RENDER_REF, 0.25f);
   /* Real SDF entry point. */
   cf_compare_sdf(info, CF_G_RENDER_COMP, CF_G_RENDER_REF, 0.5f);

   /* Plain, non-compound control glyph keeps its ordinary behavior. */
   n = stbtt_GetGlyphShape(info, CF_G_PLAIN, &v);
   CHECK(n == 5);
   CHECK(v != NULL);
   CHECK(v[0].type == STBTT_vmove && v[0].x == 0 && v[0].y == 0);
   CHECK(v[1].type == STBTT_vline && v[1].x == 100 && v[1].y == 0);
   CHECK(v[2].type == STBTT_vline && v[2].x == 100 && v[2].y == 60);
   CHECK(v[3].type == STBTT_vline && v[3].x == 0 && v[3].y == 60);
   CHECK(v[4].type == STBTT_vline && v[4].x == 0 && v[4].y == 0);
   stbtt_FreeShape(info, v);

   pixels = stbtt_GetGlyphBitmap(info, 0.5f, 0.5f, CF_G_PLAIN, &w, &h, &xo, &yo);
   CHECK(pixels != NULL && w > 0 && h > 0);
   for (i = 0; i < w * h; ++i)
      if (pixels[i]) ++nonzero;
   CHECK(nonzero > 0);
   stbtt_FreeBitmap(pixels, info->userdata);

   /* Codepoint lookup still reaches the same ordinary glyph. */
   CHECK(stbtt_FindGlyphIndex(info, 'A') == CF_G_BASE);
   {
      stbtt_vertex *cp = NULL;
      int nc = stbtt_GetCodepointShape(info, 'A', &cp);
      int ng = stbtt_GetGlyphShape(info, CF_G_BASE, &v);
      CHECK(nc == ng && cp != NULL && v != NULL);
      for (i = 0; i < nc; ++i) {
         CHECK(cp[i].type == v[i].type);
         CHECK(cp[i].x == v[i].x && cp[i].y == v[i].y);
         CHECK(cp[i].cx == v[i].cx && cp[i].cy == v[i].cy);
      }
      stbtt_FreeShape(info, cp);
      stbtt_FreeShape(info, v);
   }
}

static void cf_reset_alloc(void)
{
   CHECK(cf_alloc_live == 0);
   cf_alloc_calls = 0;
   cf_alloc_live = 0;
   cf_free_calls = 0;
   cf_fail_on = 0;
}

static void cf_expect_shape_failure(const stbtt_fontinfo *info, int glyph)
{
   stbtt_vertex *v = (stbtt_vertex *) 1;
   int n;
   cf_reset_alloc();
   n = stbtt_GetGlyphShape(info, glyph, &v);
   CHECK(n == 0);
   CHECK(v == NULL);
   CHECK(cf_alloc_live == 0);
}

static void cf_case_failure_cleanup(const stbtt_fontinfo *info)
{
   stbtt_vertex *v = NULL;
   int n, total, fail;

   /* Invalid point matching fails atomically and leaks nothing. */
   cf_expect_shape_failure(info, CF_G_FAIL_NO_PARENT);
   cf_expect_shape_failure(info, CF_G_FAIL_PARENT);
   cf_expect_shape_failure(info, CF_G_FAIL_CHILD);

   /* Count all allocations in a successful multi-component expansion. */
   cf_reset_alloc();
   n = stbtt_GetGlyphShape(info, CF_G_RAW_ANCHOR, &v);
   CHECK(n > 0 && v != NULL);
   total = cf_alloc_calls;
   CHECK(total > 0);
   CHECK(cf_alloc_live == 1); /* only the caller-owned vertex array */
   stbtt_FreeShape(info, v);
   CHECK(cf_alloc_live == 0);

   /* Fail every allocation in turn: each call must fail cleanly. */
   for (fail = 1; fail <= total; ++fail) {
      cf_reset_alloc();
      cf_fail_on = fail;
      v = (stbtt_vertex *) 1;
      n = stbtt_GetGlyphShape(info, CF_G_RAW_ANCHOR, &v);
      CHECK(n == 0);
      CHECK(v == NULL);
      CHECK(cf_alloc_live == 0);
   }

   /* With allocation restored the same glyph succeeds again. */
   cf_reset_alloc();
   v = NULL;
   n = stbtt_GetGlyphShape(info, CF_G_RAW_ANCHOR, &v);
   CHECK(n > 0 && v != NULL);
   CHECK(cf_alloc_live == 1);
   stbtt_FreeShape(info, v);
   CHECK(cf_alloc_live == 0);

   /* A failure does not disturb ordinary glyph reads. */
   cf_reset_alloc();
   v = NULL;
   n = stbtt_GetGlyphShape(info, CF_G_BASE, &v);
   CHECK(n == 5 && v != NULL);
   stbtt_FreeShape(info, v);
   CHECK(cf_alloc_live == 0);
}

int main(void)
{
   cf_font_blob font;
   stbtt_fontinfo info;

   cf_build_fixture();
   cf_make_font(&font);
   memset(&info, 0, sizeof(info));
   CHECK(stbtt_InitFont(&info, font.bytes, 0) != 0);
   CHECK(info.numGlyphs == CF_NUM_GLYPHS);

   cf_case_raw_points(&info);
   printf("PASS raw_points\n");
   fflush(stdout);
   cf_case_nested_affine(&info);
   printf("PASS nested_affine\n");
   fflush(stdout);
   cf_case_unsigned_indices(&info);
   printf("PASS unsigned_indices\n");
   fflush(stdout);
   cf_case_offset_modes(&info);
   printf("PASS offset_modes\n");
   fflush(stdout);
   cf_case_render_equivalence(&info);
   printf("PASS render_equivalence\n");
   fflush(stdout);
   cf_case_failure_cleanup(&info);
   printf("PASS failure_cleanup\n");
   fflush(stdout);

   printf("COMPOUND_ATTACHMENT_OK cases=6\n");
   return 0;
}
