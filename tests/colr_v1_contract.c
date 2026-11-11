/* SPDX-License-Identifier: MIT
 *
 * Contract tests for stbtt_MakeGlyphCOLRBitmap(). Compilable as C99 and as
 * C++11. Assertions are hand-rolled so they survive -DNDEBUG, and every group
 * prints "PASS <group> checks=<n>" only after executing real checks.
 *
 * The independent reference formulas below are written from the OpenType
 * COLR/CPAL specification directly; they do not call into the engine under
 * test except where noted for the coverage comparison.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define CHECK(cond) do { \
   ++g_checks; \
   if (!(cond)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed = 1; } \
} while (0)

static unsigned char *load_file(const char *path, size_t *out_size)
{
   FILE *f = fopen(path, "rb");
   unsigned char *buf;
   long n;
   if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0) { fclose(f); exit(2); }
   buf = (unsigned char *) malloc((size_t) n);
   if (!buf) exit(2);
   if (fread(buf, 1, (size_t) n, f) != (size_t) n) exit(2);
   fclose(f);
   *out_size = (size_t) n;
   return buf;
}

static const unsigned char *px(const unsigned char *rgba, int stride, int x, int y, int c)
{
   return rgba + (size_t) y * stride + (size_t) x * 4 + c;
}

static int near_byte(int a, int b) { int d = a - b; return d >= -2 && d <= 2; }

static void expect_pixel(const unsigned char *rgba, int stride, int x, int y,
                         int r, int g, int b, int a, const char *tag)
{
   const unsigned char *p = rgba + (size_t) y * stride + (size_t) x * 4;
   if (!near_byte(p[0], r) || !near_byte(p[1], g) || !near_byte(p[2], b) || !near_byte(p[3], a)) {
      fprintf(stderr, "pixel mismatch %s at (%d,%d): got %d,%d,%d,%d want %d,%d,%d,%d\n",
              tag, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
      g_failed = 1;
   }
   ++g_checks;
}

/* ---- independent double reference math -------------------------------- */

typedef struct { double a, b, c, d, e, f; } RefAffine; /* same convention as the spec */

static void ref_apply(const RefAffine *m, double x, double y, double *ox, double *oy)
{
   *ox = m->a*x + m->c*y + m->e;
   *oy = m->b*x + m->d*y + m->f;
}

static RefAffine ref_mul(RefAffine o, RefAffine i)
{
   RefAffine r;
   r.a = o.a*i.a + o.c*i.b;
   r.b = o.b*i.a + o.d*i.b;
   r.c = o.a*i.c + o.c*i.d;
   r.d = o.b*i.c + o.d*i.d;
   r.e = o.a*i.e + o.c*i.f + o.e;
   r.f = o.b*i.e + o.d*i.f + o.f;
   return r;
}

static int ref_invert(const RefAffine *m, RefAffine *inv)
{
   double det = m->a*m->d - m->b*m->c;
   double k;
   if (det == 0.0) return 0;
   k = 1.0 / det;
   inv->a =  m->d * k;
   inv->b = -m->b * k;
   inv->c = -m->c * k;
   inv->d =  m->a * k;
   inv->e = (m->c*m->f - m->d*m->e) * k;
   inv->f = (m->b*m->e - m->a*m->f) * k;
   return 1;
}

/* three-point linear gradient parameter t at a canvas pixel, PAD clamped */
static double ref_gradient_t(const RefAffine *paint_inv,
                             double px, double py,
                             double x0, double y0, double x1, double y1,
                             double x2, double y2)
{
   double qx, qy, vx, vy;
   double dx = x1 - x0, dy = y1 - y0;
   double ex = x2 - x0, ey = y2 - y0;
   double dd = dx*dx + dy*dy;
   double ee = ex*ex + ey*ey;
   double de = dx*ex + dy*ey;
   double denom = dd*ee - de*de;
   double t;
   ref_apply(paint_inv, px, py, &qx, &qy);
   vx = qx - x0; vy = qy - y0;
   t = ((vx*dx + vy*dy) * ee - (vx*ex + vy*ey) * de) / denom;
   if (t < 0.0) t = 0.0;
   if (t > 1.0) t = 1.0;
   return t;
}

static unsigned char ref_quant(double v)
{
   double q = v * 255.0 + 0.5;
   if (q <= 0.0) return 0;
   if (q >= 255.0) return 255;
   return (unsigned char) q;
}

/* premultiplied SRC_OVER in straight sRGB components, returns bytes RGBA */
static void ref_over(double sr, double sg, double sb, double sa,
                     double dr, double dg, double db, double da,
                     unsigned char out[4])
{
   double ia = 1.0 - sa;
   double ar = sr + dr * ia, ag = sg + dg * ia, ab = sb + db * ia;
   double aa = sa + da * ia;
   out[0] = ref_quant(ar); out[1] = ref_quant(ag); out[2] = ref_quant(ab); out[3] = ref_quant(aa);
}

static const int W = 48, H = 48;
static const float S = 0.2f, SX = 4.0f, SY = 24.0f;

static stbtt_fontinfo open_font(const char *path, unsigned char **buf_keep)
{
   size_t n;
   stbtt_fontinfo info;
   *buf_keep = load_file(path, &n);
   if (!stbtt_InitFont(&info, *buf_keep, 0)) { fprintf(stderr, "InitFont failed %s\n", path); exit(2); }
   return info;
}

static unsigned char *new_canvas(int extra_stride, int *stride_out)
{
   int stride = W * 4 + extra_stride;
   unsigned char *p = (unsigned char *) malloc((size_t) stride * H);
   *stride_out = stride;
   return p;
}

/* ====================================================================== */
static void case_gradient_transform(const char *font_path)
{
   unsigned char *buf;
   stbtt_fontinfo info = open_font(font_path, &buf);
   int stride;
   unsigned char *a = new_canvas(0, &stride);
   unsigned char *b = new_canvas(0, &stride);
   unsigned char fg[4] = {10, 20, 30, 200};
   int x, y, differing_pixels, partial_edge;

   /* g5: transform OUTSIDE PaintGlyph (outline + field move together) */
   memset(a, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, a, W, H, stride, S, S, SX, SY, 5, 0, fg) == 1);
   /* g6: transform wraps the FILL only; outline is the plain g2 square */
   memset(b, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, b, W, H, stride, S, S, SX, SY, 6, 0, fg) == 1);

   /* the two contexts must not paint the same alpha mask */
   differing_pixels = 0;
   for (y = 0; y < H; ++y)
      for (x = 0; x < W; ++x)
         if (*px(a, stride, x, y, 3) != *px(b, stride, x, y, 3)) ++differing_pixels;
   CHECK(differing_pixels > 0);

   /* g6 outline stays at the untransformed g2 square: interior pixel (14,14)
    * is fully covered; g5's moved outline must still cover (14,14) too */
   CHECK(*px(b, stride, 14, 14, 3) == 255);
   CHECK(*px(a, stride, 14, 14, 3) > 0);

   /* independent three-point gradient at an interior pixel of g6.
    * paint matrix is the root mapping plus the fill-only translation (12,-8) */
   {
      RefAffine root = { S, 0, 0, -S, SX, SY };
      RefAffine t6 = { 1, 0, 0, 1, 12, -8 };
      RefAffine paint = ref_mul(root, t6), inv;
      double t, r, gch;
      CHECK(ref_invert(&paint, &inv));
      /* g2 square interior, well away from antialiased edges */
      t = ref_gradient_t(&inv, 14.5, 14.5, 0, 0, 100, 0, 50, 100);
      r = 1.0 - t; gch = t; /* palette 0 stops: opaque red -> opaque green */
      expect_pixel(b, stride, 14, 14, ref_quant(r), ref_quant(gch), 0, 255, "g6 gradient interior");
      /* a second, distinct interior pixel must give a different t, proving
       * the p2 point participates rather than a simple p0->p1 projection */
      {
         double t2 = ref_gradient_t(&inv, 10.5, 18.5, 0, 0, 100, 0, 50, 100);
         CHECK(fabs(t2 - t) > 1e-6);
         expect_pixel(b, stride, 10, 18, ref_quant(1.0 - t2), ref_quant(t2), 0, 255, "g6 gradient second");
      }
   }

   /* fractional outline transform must leave a partial-coverage edge pixel,
    * i.e. the contour was not snapped to integer canvas coordinates */
   partial_edge = 0;
   for (y = 0; y < H; ++y)
      for (x = 0; x < W; ++x) {
         unsigned char al = *px(a, stride, x, y, 3);
         if (al != 0 && al != 255) { partial_edge = 1; }
      }
   CHECK(partial_edge);

   /* g7: shared graph under two non-commuting nested matrices; the two
    * transformed squares must cover different canvas regions */
   {
      unsigned char *c = new_canvas(0, &stride);
      RefAffine root = { S, 0, 0, -S, SX, SY };
      RefAffine ma = { 1, 0.25, 0, 1, 0, 0 };
      RefAffine mb = { 0.75, 0, 0.3, 0.75, 20, 10 };
      RefAffine ab = ref_mul(root, ref_mul(mb, ma));
      RefAffine ba = ref_mul(root, ref_mul(ma, mb));
      double corners[4][2] = {{0,0},{100,0},{100,100},{0,100}};
      double abx0 = 1e9, aby0 = 1e9, abx1 = -1e9, aby1 = -1e9;
      double bax0 = 1e9, bay0 = 1e9, bax1 = -1e9, bay1 = -1e9;
      int asymmetric = 0, k;
      memset(c, 0, (size_t) stride * H);
      CHECK(stbtt_MakeGlyphCOLRBitmap(&info, c, W, H, stride, S, S, SX, SY, 7, 0, fg) == 1);
      for (k = 0; k < 4; ++k) {
         double X, Y;
         ref_apply(&ab, corners[k][0], corners[k][1], &X, &Y);
         if (X < abx0) abx0 = X; if (X > abx1) abx1 = X;
         if (Y < aby0) aby0 = Y; if (Y > aby1) aby1 = Y;
         ref_apply(&ba, corners[k][0], corners[k][1], &X, &Y);
         if (X < bax0) bax0 = X; if (X > bax1) bax1 = X;
         if (Y < bay0) bay0 = Y; if (Y > bay1) bay1 = Y;
      }
      /* matrix products genuinely differ (the inner 2x2 parts do not commute) */
      CHECK(fabs(ab.a - ba.a) > 1e-9 || fabs(ab.b - ba.b) > 1e-9 ||
            fabs(ab.c - ba.c) > 1e-9 || fabs(ab.d - ba.d) > 1e-9);
      /* a painted pixel must exist inside the ab box but outside the ba box */
      for (y = 0; y < H && !asymmetric; ++y)
         for (x = 0; x < W; ++x) {
            double cxp = x + 0.5, cyp = y + 0.5;
            int in_ab = cxp >= abx0 && cxp <= abx1 && cyp >= aby0 && cyp <= aby1;
            int in_ba = cxp >= bax0 && cxp <= bax1 && cyp >= bay0 && cyp <= bay1;
            if (in_ab && !in_ba && *px(c, stride, x, y, 3) > 0) { asymmetric = 1; break; }
         }
      CHECK(asymmetric);
      free(c);
   }

   free(a); free(b); free(buf);
   if (!g_failed) printf("PASS gradient_transform checks=%d\n", g_checks);
}

/* ====================================================================== */
static void case_isolated_composite(const char *font_path)
{
   unsigned char *buf;
   stbtt_fontinfo info = open_font(font_path, &buf);
   int stride;
   unsigned char *img = new_canvas(0, &stride);
   unsigned char fg[4] = {10, 20, 30, 200};
   int x, y, saw_intersection;

   /* ---- g9: outer sibling g4(green) + SRC_IN of g3 gradient inside g2 ---- */
   memset(img, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, img, W, H, stride, S, S, SX, SY, 9, 0, fg) == 1);

   /* g4-only region (half square x[4,12], outside the g3 inset x[8,16])
    * holds the green sibling and is unaffected by the composite */
   expect_pixel(img, stride, 5, 22, 0, 255, 0, 255, "g9 sibling green");
   /* the SRC_IN result is confined to the g3 inset x[8,16], y[12,20];
    * a pixel inside g2 but outside the g3 source must keep no source color */
   CHECK(*px(img, stride, 20, 14, 3) == 0 || *px(img, stride, 20, 14, 1) == 0);
   /* intersection pixel carries source alpha multiplied by backdrop alpha */
   saw_intersection = 0;
   for (y = 12; y < 20 && !saw_intersection; ++y)
      for (x = 8; x < 16; ++x)
         if (*px(img, stride, x, y, 3) > 0) { saw_intersection = 1; break; }
   CHECK(saw_intersection);

   /* ---- g10: half-alpha SRC_OVER, checked against an independent formula ---- */
   memset(img, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, img, W, H, stride, S, S, SX, SY, 10, 0, fg) == 1);
   {
      /* g3 inset square interior (14,14): source index1 half alpha blue,
       * backdrop g2 index1 half alpha blue; both resolve to the same color
       * in palette 0: straight (0,0,1) alpha 0.5 */
      unsigned char want[4];
      double col = 1.0, half = 0.5;
      /* premultiplied source over premultiplied backdrop, identical color */
      ref_over(0.0, 0.0, col*half, half, 0.0, 0.0, col*half, half, want);
      expect_pixel(img, stride, 14, 14, want[0], want[1], want[2], want[3], "g10 SRC_OVER half alpha");
   }

   /* ---- g11: clip restore. g3 clip ends, then g4 paints its own square ---- */
   memset(img, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, img, W, H, stride, S, S, SX, SY, 11, 0, fg) == 1);
   /* pixel inside g4 (x[4,12]) but outside g3 inset (x[8,16]) is pure green */
   expect_pixel(img, stride, 5, 10, 0, 255, 0, 255, "g11 clip restored green");
   /* pixel inside g3 keeps the blue-half fill, proving the first clip applied */
   {
      const unsigned char *p = img + (size_t) 14 * stride + (size_t) 14 * 4;
      CHECK(p[3] > 0 && p[2] > 0 && p[0] == 0 && p[1] == 0);
   }

   free(img); free(buf);
   if (!g_failed) printf("PASS isolated_composite checks=%d\n", g_checks);
}

/* ====================================================================== */
static void case_shared_graph(const char *font_path)
{
   unsigned char *buf;
   stbtt_fontinfo info = open_font(font_path, &buf);
   int stride;
   unsigned char *p0 = new_canvas(0, &stride);
   unsigned char *p1 = new_canvas(0, &stride);
   unsigned char *p2 = new_canvas(0, &stride);
   unsigned char fg_a[4] = {10, 20, 30, 200};
   unsigned char fg_b[4] = {200, 100, 50, 255};
   int x, y, changed;

   /* same shared graph, palette 0 vs palette 1, then palette 0 again with a
    * different caller foreground color */
   memset(p0, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, p0, W, H, stride, S, S, SX, SY, 8, 0, fg_a) == 1);
   memset(p1, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, p1, W, H, stride, S, S, SX, SY, 8, 1, fg_a) == 1);
   memset(p2, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&info, p2, W, H, stride, S, S, SX, SY, 8, 0, fg_b) == 1);

   /* palette swap changes the red stop region (interior pixel near t=0) */
   changed = 0;
   for (y = 0; y < H; ++y)
      for (x = 0; x < W; ++x)
         if (memcmp(p0 + (size_t) y * stride + (size_t) x * 4,
                    p1 + (size_t) y * stride + (size_t) x * 4, 4) != 0) ++changed;
   CHECK(changed > 0);

   /* the foreground color drives the t=1 stop: an interior pixel near p1 in
    * p2 must reflect fg_b premultiplied, distinct from fg_a in p0 */
   {
      int diff = 0;
      for (y = 0; y < H; ++y)
         for (x = 0; x < W; ++x)
            if (memcmp(p0 + (size_t) y * stride + (size_t) x * 4,
                       p2 + (size_t) y * stride + (size_t) x * 4, 4) != 0) ++diff;
      CHECK(diff > 0);
   }

   /* glyph 7 references the very same shared subgraph twice in one call, under
    * distinct transform contexts; both references must actually paint, and the
    * shared graph must not be cached from the first reference for the second */
   {
      unsigned char *q = new_canvas(0, &stride);
      int painted = 0;
      memset(q, 0, (size_t) stride * H);
      CHECK(stbtt_MakeGlyphCOLRBitmap(&info, q, W, H, stride, S, S, SX, SY, 7, 0, fg_a) == 1);
      for (y = 0; y < H; ++y)
         for (x = 0; x < W; ++x)
            if (*px(q, stride, x, y, 3) > 0) ++painted;
      CHECK(painted > 0);
      /* g8 paints the shared graph once; g7 must not equal that single render */
      CHECK(memcmp(q, p0, (size_t) stride * H) != 0);
      free(q);
   }

   /* a repeat render with identical arguments must be byte-for-byte stable:
    * no cross-call state or reused pixels from an earlier context */
   {
      unsigned char *p3 = new_canvas(0, &stride);
      memset(p3, 0, (size_t) stride * H);
      CHECK(stbtt_MakeGlyphCOLRBitmap(&info, p3, W, H, stride, S, S, SX, SY, 8, 0, fg_a) == 1);
      CHECK(memcmp(p0, p3, (size_t) stride * H) == 0);
      free(p3);
   }

   /* foreground stop check against an independent formula: find an interior
    * pixel with t close to 1 and compare premultiplied fg bytes */
   {
      RefAffine root = { S, 0, 0, -S, SX, SY }, inv;
      CHECK(ref_invert(&root, &inv));
      /* scan the g2 square interior for the largest-t fully covered pixel */
      double best_t = -1.0; int bx = -1, by = -1;
      for (y = 5; y < 23; ++y)
         for (x = 5; x < 23; ++x) {
            if (*px(p2, stride, x, y, 3) != 255) continue;
            {
               double tt = ref_gradient_t(&inv, x + 0.5, y + 0.5, 0, 0, 100, 0, 50, 100);
               if (tt > best_t) { best_t = tt; bx = x; by = y; }
            }
         }
      CHECK(bx >= 0 && best_t > 0.75);
      /* line2 blends opaque red at t=0 into the caller foreground at t=1,
       * interpolated premultiplied (fg_b is opaque here) */
      {
         double fr = fg_b[0] / 255.0, fg = fg_b[1] / 255.0, fb = fg_b[2] / 255.0;
         double er = (1.0 - best_t) * 1.0 + best_t * fr;
         double eg = best_t * fg;
         double eb = best_t * fb;
         expect_pixel(p2, stride, bx, by, ref_quant(er), ref_quant(eg), ref_quant(eb), 255,
                      "shared foreground stop");
      }
   }

   free(p0); free(p1); free(p2); free(buf);
   if (!g_failed) printf("PASS shared_graph checks=%d\n", g_checks);
}

/* ====================================================================== */
static void case_legacy_and_errors(const char *contract_path, const char *geometry_path,
                                   const char *errors_path)
{
   unsigned char *cbuf, *gbuf, *ebuf;
   stbtt_fontinfo cinfo = open_font(contract_path, &cbuf);
   stbtt_fontinfo ginfo = open_font(geometry_path, &gbuf);
   stbtt_fontinfo einfo = open_font(errors_path, &ebuf);
   int stride, sentinel_stride;
   unsigned char *img = new_canvas(0, &stride);
   unsigned char *sent = new_canvas(8, &sentinel_stride);
   unsigned char fg[4] = {11, 22, 33, 255};
   unsigned char pattern[4] = {0x5b, 0x1e, 0x77, 0xd0};
   int x, y, max_diff;

   /* ---- alpha parity with the legacy grayscale rasterizer (g2 square) ---- */
   {
      int gw, gh, gx, gy, diff;
      unsigned char *gray;
      memset(img, 0, (size_t) stride * H);
      CHECK(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, S, S, SX, SY, 1, 0, fg) == 1);
      /* identical scale and shift so the grayscale bitmap aligns 1:1 with
       * the COLR canvas; glyph 1's outer opaque layer is the g2 square */
      gray = stbtt_GetGlyphBitmapSubpixel(&cinfo, S, S, SX, SY, 2, &gw, &gh, &gx, &gy);
      CHECK(gray != NULL && gw > 0 && gh > 0);
      max_diff = 0;
      for (y = 0; y < gh; ++y)
         for (x = 0; x < gw; ++x) {
            int cxp = x + gx, cyp = y + gy;
            if (cxp < 0 || cyp < 0 || cxp >= W || cyp >= H) continue;
            diff = (int) gray[y * gw + x] - (int) *px(img, stride, cxp, cyp, 3);
            if (diff < 0) diff = -diff;
            if (diff > max_diff) max_diff = diff;
         }
      CHECK(max_diff <= 1);
      stbtt_FreeBitmap(gray, NULL);
   }

   /* ---- SDF entry point still behaves ---- */
   {
      int sw, sh, sx0, sy0;
      unsigned char *sdf = stbtt_GetGlyphSDF(&ginfo, 0.05f, 2, 4, 128, 32, &sw, &sh, &sx0, &sy0);
      CHECK(sdf != NULL && sw > 0 && sh > 0);
      stbtt_FreeSDF(sdf, NULL);
   }

   /* ---- stride sentinels are never touched, interior is cleared ---- */
   for (y = 0; y < H; ++y)
      memset(sent + (size_t) y * sentinel_stride, 0x5b, (size_t) sentinel_stride);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&cinfo, sent, W, H, sentinel_stride, S, S, SX, SY, 1, 0, fg) == 1);
   for (y = 0; y < H; ++y)
      for (x = W * 4; x < sentinel_stride; ++x)
         CHECK(sent[(size_t) y * sentinel_stride + x] == 0x5b);

   /* ---- failures leave the whole output untouched ---- */
   #define EXPECT_FAIL(call) do { \
      for (y = 0; y < H; ++y) { \
         int xx; for (xx = 0; xx < W; ++xx) memcpy(img + (size_t)y*stride + (size_t)xx*4, pattern, 4); \
      } \
      CHECK((call) == 0); \
      for (y = 0; y < H; ++y) { \
         int xx; for (xx = 0; xx < W; ++xx) \
            CHECK(memcmp(img + (size_t)y*stride + (size_t)xx*4, pattern, 4) == 0); \
      } \
   } while (0)

   /* ordinary glyph without a color root */
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&ginfo, img, W, H, stride, S, S, SX, SY, 2, 0, fg));
   /* unknown palette */
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, S, S, SX, SY, 1, 99, fg));
   /* out of range glyph */
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, S, S, SX, SY, 9999, 0, fg));
   /* self cycle, missing reference, unsupported format, unsupported mode */
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&einfo, img, W, H, stride, S, S, SX, SY, 2, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&einfo, img, W, H, stride, S, S, SX, SY, 3, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&einfo, img, W, H, stride, S, S, SX, SY, 4, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&einfo, img, W, H, stride, S, S, SX, SY, 5, 0, fg));
   /* invalid geometry / pointer arguments */
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, 0, H, stride, S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, -1, stride, S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, W * 4 - 1, S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, 0.0f, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, -S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, INFINITY, S, SX, SY, 1, 0, fg));
   {
      float nanv = NAN;
      EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, S, nanv, SX, SY, 1, 0, fg));
   }
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(NULL, img, W, H, stride, S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, NULL, W, H, stride, S, S, SX, SY, 1, 0, fg));
   EXPECT_FAIL(stbtt_MakeGlyphCOLRBitmap(&cinfo, img, W, H, stride, S, S, SX, SY, 1, 0, NULL));
   #undef EXPECT_FAIL

   /* the well-formed base in the errors font still renders */
   memset(img, 0, (size_t) stride * H);
   CHECK(stbtt_MakeGlyphCOLRBitmap(&einfo, img, W, H, stride, S, S, SX, SY, 6, 0, fg) == 1);

   free(img); free(sent); free(cbuf); free(gbuf); free(ebuf);
   if (!g_failed) printf("PASS legacy_and_errors checks=%d\n", g_checks);
}

int main(int argc, char **argv)
{
   static const char contract[] = "colrcheck/fixtures/contract.ttf";
   static const char geometry[] = "colrcheck/geometry.ttf";
   static const char errors[]   = "colrcheck/fixtures/errors.ttf";
   const char *group;
   if (argc != 3 || strcmp(argv[1], "--case") != 0) {
      fprintf(stderr, "usage: %s --case gradient_transform|isolated_composite|shared_graph|legacy_and_errors\n", argv[0]);
      return 2;
   }
   group = argv[2];

   g_checks = 0; g_failed = 0;
   if (strcmp(group, "gradient_transform") == 0)
      case_gradient_transform(contract);
   else if (strcmp(group, "isolated_composite") == 0)
      case_isolated_composite(contract);
   else if (strcmp(group, "shared_graph") == 0)
      case_shared_graph(contract);
   else if (strcmp(group, "legacy_and_errors") == 0)
      case_legacy_and_errors(contract, geometry, errors);
   else {
      fprintf(stderr, "unknown case %s\n", group);
      return 2;
   }
   return g_failed ? 1 : 0;
}
