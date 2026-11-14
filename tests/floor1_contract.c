/*
 * tests/floor1_contract.c
 *
 * Floor 1 numerical regression for stb_vorbis.  Everything in this file is
 * self-contained C99: streams are synthesized bit-by-bit, Ogg pages carry
 * real CRCs, and every "expected" value comes from an independent model of
 * the Vorbis I specification written below -- never from the production
 * decoder's predict_point()/draw_line()/do_floor()/inverse_mdct() output.
 *
 * Three groups are exported through task_cases:
 *
 *   points  - independent Floor1 control-point recovery (neighbor search,
 *             descending/truncated prediction, odd/even deltas, unpacking
 *             past the symmetric range, zero-delta points that get revived
 *             as neighbors) compared against the live decoder state.
 *   curve   - independent integer Bresenham floor curves (truncated signed
 *             division, rising/falling segments, exact integer threshold
 *             crossings, endpoint ownership, tail hold, clipping to the
 *             half-block) compared bin-by-bin against production do_floor()
 *             products with a non-constant, non-zero residue.
 *   packets - end-to-end decode of two hand-built Ogg streams (256- and
 *             512-sample blocks, mono, no coupling) containing non-flat
 *             interior floor points, non-zero residue and a floor-less
 *             packet sandwiched between active ones; PCM is predicted with
 *             a direct-cosine IMDCT and explicit windowed overlap-add.
 *
 * Why the baseline (task_support/baseline.c + tiny_stream.h) cannot cover
 * this: its only floor has the two implicit endpoints with identical Y and
 * no interior points, so predict_point() never interpolates, every floor
 * curve is a single flat line, no packet ever has the floor unused flag,
 * and the overlap-add never mixes a silent-spectrum frame with a previous
 * packet's tail.  The mechanisms below are therefore all unexamined by the
 * existing PCM smoke test.
 *
 * Independent-model basis:
 *   - Point recovery follows the Vorbis I "floor type 1" decode: neighbors
 *     are chosen only from points already declared (setup X order, not
 *     frequency order), prediction is truncated rational interpolation,
 *     and the raw scalar is unwrapped with the odd/even symmetric rule and
 *     the two out-of-range branches; draw order is the frequency-sorted
 *     order.
 *   - The line model is the specification's integer Bresenham variant
 *     (base = trunc(dy/adx), residual error stepped against adx), checked
 *     against a second, closed-form truncated-rational derivation so the
 *     integer Y comparisons below are exact, not tolerance based.
 *   - The transform model is the direct cosine sum
 *       y[i] = sum_j X[j] cos(pi/(2n) (2i+1+n/2)(2j+1))
 *     (the same definition stb_vorbis keeps in its #if 0 reference IMDCT),
 *     followed by the Vorbis power-of-two sine window and explicit
 *     overlap-add of the previous block's un-windowed right half.
 *
 * First divergence points for the two shipped mutations (documented here so
 * the failure locations are auditable; the checks are full-range so they
 * catch them wherever they land):
 *   - slope.patch flips the descending branch of predict_point(); point
 *     recovery case "recovery" reaches it first at declared point 7
 *     (X=60), whose prediction 143 = 145 - trunc(25/9) becomes 147 under
 *     the sign flip, so the decoder's finalY[7] no longer equals the
 *     model's 140.  Every packet stream with a descending prediction is
 *     affected the same way.
 *   - stair.patch changes err >= adx to err > adx in draw_line(); the
 *     first affected bin is x=15 on the falling segment (12,100)->(30,70),
 *     where the residual lands exactly on adx (12*(15-12) == 18*2): the
 *     correct stepped Y 95 is produced as 96, and the full-range curve
 *     comparison in case "segments" fails there.  The long clipped
 *     segment (40,130)->(150,200) reaches the same condition again at
 *     x=51, and the end-to-end packet streams diverge the same way.
 */
#include "check.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

/* ===================================================================== */
/* shared helpers                                                        */
/* ===================================================================== */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fl1_ilog(int n)
{
   int r = 0;
   if (n < 0) return 0;
   while (n) { n >>= 1; ++r; }
   return r;
}

static unsigned fl1_revbits(unsigned v, int bits)
{
   unsigned r = 0;
   int i;
   for (i = 0; i < bits; ++i) { r = (r << 1) | (v & 1u); v >>= 1; }
   return r;
}

static int fl1_near(double got, double want, double tol_scale)
{
   double tol = tol_scale * (1.0 + fabs(want));
   return fabs(got - want) <= tol;
}

/* float32_pack(0.5) used by the residue value book builder. */
static unsigned fl1_pack_half(void);

/* ===================================================================== */
/* independent Floor1 point model                                        */
/* ===================================================================== */

#define FL1_MAX_PTS 64

typedef struct
{
   int x;                       /* frequency coordinate (declaration set) */
   int y;                       /* recovered Y, valid for every point     */
   int active;                  /* participates in curve drawing          */
} fl1_point;

/* Truncated rational prediction, written straight from the spec rather
 * than calling production predict_point(): integer division truncates the
 * absolute offset toward zero, then the sign of (y1-y0) is applied. */
static int fl1_model_predict(int x, int x0, int y0, int x1, int y1)
{
   int dy = y1 - y0;
   int adx = x1 - x0;
   int off = (int) ((long) abs(dy) * (x - x0) / adx);
   return dy < 0 ? y0 - off : y0 + off;
}

/* Vorbis I neighbor rule: only points with index < j are candidates. */
static void fl1_model_neighbors(const int *X, int j, int *low, int *high)
{
   int i, lo = -1, hi = -1;
   for (i = 0; i < j; ++i) {
      if (X[i] < X[j] && (lo < 0 || X[i] > X[lo])) lo = i;
      if (X[i] > X[j] && (hi < 0 || X[i] < X[hi])) hi = i;
   }
   *low = lo;
   *high = hi;
}

/* Inverse of the production floor scalar unwrap, derived independently. */
static int fl1_model_unwrap(int val, int pred, int range)
{
   int highroom = range - pred;
   int lowroom = pred;
   int room = highroom < lowroom ? highroom * 2 : lowroom * 2;
   if (val == 0)
      return pred;
   if (val >= room) {
      if (highroom > lowroom)
         return val - lowroom + pred;
      return pred - val + highroom - 1;
   }
   if (val & 1)
      return pred - ((val + 1) >> 1);
   return pred + (val >> 1);
}

/* Forward encoder used by the packet builders: given the Y we want on the
 * wire, produce a legal raw scalar for the decoder above. */
static int fl1_model_encode(int target, int pred, int range)
{
   int highroom = range - pred;
   int lowroom = pred;
   int d, a;
   if (target == pred) return 0;
   d = target - pred;
   if (highroom > lowroom) {
      /* symmetric room spans 2*lowroom */
      if (d > 0 && d < lowroom) return 2 * d;            /* even */
      if (d < 0 && -d < lowroom) return 2 * (-d) - 1;    /* odd  */
      return target;                                     /* outer branch */
   } else {
      if (d > 0 && d < highroom) return 2 * d;
      if (d < 0 && (a = -d) < highroom) return 2 * a - 1;
      /* outer branch: target = pred - val + highroom - 1 */
      return pred + highroom - 1 - target;
   }
}

/* Full point recovery for one declared set; X[0],X[1] are the endpoints.
 * raw[] holds wire scalars (raw[0],raw[1] are the endpoint Y values). */
static void fl1_model_recover(const int *X, const int *raw, int count,
                              int range, fl1_point *out)
{
   int j;
   for (j = 0; j < count; ++j) {
      out[j].x = X[j];
      out[j].active = 0;
   }
   out[0].y = raw[0];
   out[1].y = raw[1];
   out[0].active = out[1].active = 1;
   for (j = 2; j < count; ++j) {
      int low, high, pred;
      fl1_model_neighbors(X, j, &low, &high);
      pred = fl1_model_predict(X[j], X[low], out[low].y, X[high], out[high].y);
      out[j].y = fl1_model_unwrap(raw[j], pred, range);
      if (raw[j] != 0) {
         out[j].active = 1;
         out[low].active = 1;
         out[high].active = 1;
      }
   }
}

/* Frequency-sorted draw order of the active points, modeled after the
 * production sorted_order + finalY >= 0 walk. */
static int fl1_model_draw_order(const fl1_point *pts, int count, int *order)
{
   int n = 0, i, k;
   for (i = 0; i < count; ++i)
      if (pts[i].active) order[n++] = i;
   for (i = 0; i < n; ++i)
      for (k = i + 1; k < n; ++k)
         if (pts[order[k]].x < pts[order[i]].x) {
            int t = order[i]; order[i] = order[k]; order[k] = t;
         }
   return n;
}

/* ===================================================================== */
/* independent integer line / floor-curve model                          */
/* ===================================================================== */

/* Bresenham Y at bin x for segment (x0,y0)->(x1,y1), implemented directly
 * from the spec's error-accumulation description.  Returns the integer Y
 * (caller masks to the dB table). */
static int fl1_line_step(int x, int x0, int y0, int x1, int y1)
{
   int dy = y1 - y0;
   int adx = x1 - x0;
   int ady = abs(dy);
   int base = dy / adx;                 /* truncates toward zero */
   int sy = dy < 0 ? base - 1 : base + 1;
   int err = 0, y = y0, xx;
   ady -= abs(base) * adx;
   for (xx = x0 + 1; xx <= x; ++xx) {
      err += ady;
      if (err >= adx) { err -= adx; y += sy; }
      else            y += base;
   }
   return y;
}

/* Second, independent derivation: truncated rational interpolation.
 * Every integer Y assertion in the tests compares both derivations, so the
 * integer path is exact rather than tolerance based. */
static int fl1_line_rational(int x, int x0, int y0, int x1, int y1)
{
   long num = (long) (y1 - y0) * (x - x0);
   int den = x1 - x0;
   int q = (int) (num / den);          /* C division truncates toward 0 */
   return y0 + q;
}

/* Build the complete integer floor curve over n2 bins from active points.
 * Points past n2 are clipped; the final value is held to the end. */
static void fl1_model_curve(const fl1_point *pts, int count, int multiplier,
                            int n2, int *ycurve)
{
   int order[FL1_MAX_PTS];
   int n = fl1_model_draw_order(pts, count, order);
   int lx = pts[order[0]].x;
   int ly = pts[order[0]].y * multiplier;
   int q, x, x1, y1;
   /* the implicit left endpoint always sits at x=0 */
   for (x = 0; x < lx && x < n2; ++x) ycurve[x] = ly;
   for (q = 1; q < n; ++q) {
      x1 = pts[order[q]].x;
      y1 = pts[order[q]].y * multiplier;
      if (x1 == lx) continue;          /* right endpoint owned by next seg */
      for (x = lx; x < x1 && x < n2; ++x)
         ycurve[x] = fl1_line_step(x, lx, ly, x1, y1);
      lx = x1; ly = y1;
      if (lx >= n2) break;
   }
   for (x = lx; x < n2; ++x) ycurve[x] = ly;   /* tail hold */
}

/* ===================================================================== */
/* LSB-first bit writer and Ogg pager (style of tiny_stream.h, generic)  */
/* ===================================================================== */

typedef struct
{
   unsigned char data[4096];
   unsigned pos;
} fl1_bits;

static void fl1_put(fl1_bits *b, unsigned value, unsigned n)
{
   unsigned i;
   for (i = 0; i < n; ++i, ++b->pos) {
      if (value & (1u << i))
         b->data[b->pos >> 3] |= 1u << (b->pos & 7u);
   }
}

static void fl1_magic(fl1_bits *b, unsigned type)
{
   static const char tag[6] = { 'v','o','r','b','i','s' };
   int i;
   fl1_put(b, type, 8);
   for (i = 0; i < 6; ++i) fl1_put(b, (unsigned char) tag[i], 8);
}

static unsigned fl1_page(unsigned char *out, const fl1_bits *p,
                         unsigned seq, unsigned flags, uint64 granule)
{
   unsigned n = (p->pos + 7) >> 3;
   unsigned i, k;
   uint32_t crc = 0;
   memset(out, 0, n + 28);
   memcpy(out, "OggS", 4);
   out[4] = 0;
   out[5] = (unsigned char) flags;
   for (i = 0; i < 8; ++i) out[6 + i] = (unsigned char) (granule >> (8 * i));
   for (i = 0; i < 4; ++i) out[14 + i] = 0;       /* serial */
   for (i = 0; i < 4; ++i) out[18 + i] = (unsigned char) (seq >> (8 * i));
   out[22] = out[23] = out[24] = out[25] = 0;    /* CRC slot empty */
   out[26] = 1;                                   /* one segment table entry */
   out[27] = (unsigned char) n;
   memcpy(out + 28, p->data, n);
   for (i = 0; i < n + 28; ++i) {
      crc ^= (uint32_t) out[i] << 24;
      for (k = 0; k < 8; ++k)
         crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
   }
   for (k = 0; k < 4; ++k) out[22 + k] = (unsigned char) (crc >> (8 * k));
   return n + 28;
}

/* ===================================================================== */
/* Vorbis setup builders                                                 */
/* ===================================================================== */

/* A scalar lookup-type-0 Huffman book with `entries` power-of-two symbols
 * all of uniform length log2(entries). */
/* dimensions (16-bit) and entry count (24-bit) are little-endian byte
 * fields on the wire, not LSB-first integers of the whole width. */
static void fl1_put_book_head(fl1_bits *b, int dimensions, int entries)
{
   fl1_put(b, (unsigned) (dimensions & 0xff), 8);
   fl1_put(b, (unsigned) ((dimensions >> 8) & 0xff), 8);
   fl1_put(b, (unsigned) (entries & 0xff), 8);
   fl1_put(b, (unsigned) ((entries >> 8) & 0xff), 8);
   fl1_put(b, (unsigned) ((entries >> 16) & 0xff), 8);
}

static void fl1_put_scalar_book(fl1_bits *b, int entries)
{
   /* a 1-symbol book uses codeword-length field 0 (actual length 1) */
   int len = entries == 1 ? 1 : fl1_ilog(entries - 1);
   int i;
   /* codebooks inside the setup header carry the bare 24-bit BCV sync */
   fl1_put(b, 0x42, 8); fl1_put(b, 0x43, 8); fl1_put(b, 0x56, 8);
   fl1_put_book_head(b, 1, entries);
   fl1_put(b, 0, 1);                       /* ordered = 0 */
   fl1_put(b, 0, 1);                       /* sparse = 0 */
   for (i = 0; i < entries; ++i) fl1_put(b, (unsigned) (len - 1), 5);
   fl1_put(b, 0, 4);                       /* lookup type 0 */
}

/* Residue value book: dim 1, lookup type 1, multiplicands
 * {0.0, 0.5, 1.0} via minimum=0 delta=0.5 (three 2-bit symbols). */
static void fl1_put_residue_book(fl1_bits *b)
{
   int i;
   fl1_put(b, 0x42, 8); fl1_put(b, 0x43, 8); fl1_put(b, 0x56, 8);
   fl1_put_book_head(b, 1, 3);
   fl1_put(b, 0, 1);                       /* ordered */
   fl1_put(b, 0, 1);                       /* sparse */
   for (i = 0; i < 3; ++i) fl1_put(b, 1, 5);       /* length 2 each */
   fl1_put(b, 1, 4);                       /* lookup type 1 */
   fl1_put(b, 0, 32);                      /* minimum value 0.0 */
   fl1_put(b, fl1_pack_half(), 32);        /* delta 0.5 */
   fl1_put(b, 1, 4);                       /* value_bits = 2 (wire = bits-1) */
   fl1_put(b, 0, 1);                       /* sequence_p = 0 */
   for (i = 0; i < 3; ++i) fl1_put(b, (unsigned) i, 2);
}

/* float32_pack(0.5): mantissa 2^20, exponent field 767 (see float32_unpack). */
static unsigned fl1_pack_half(void)
{
   return (767u << 21) | (1u << 20);
}

/* Floor1 + residue + mapping + mode configuration.
 * Books: 0 = residue classbook (1 entry), 1 = residue values, 2 = floor. */
static void fl1_put_setup(fl1_bits *b, int block_log, int multiplier,
                          int rangebits, const int *interior_x, int ninterior)
{
   int i;
   fl1_magic(b, 5);

   /* codebooks (8-bit count, wire value is count-1) */
   fl1_put(b, 2, 8);
   fl1_put_scalar_book(b, 1);              /* book 0: classbook */
   fl1_put_residue_book(b);                /* book 1: residue values */
   fl1_put_scalar_book(b, 256);            /* book 2: floor scalars */

   /* time domain transforms: one unused, zero-valued record */
   fl1_put(b, 0, 6);
   fl1_put(b, 0, 16);

   /* floors: one floor type 1 (wire count is count-1) */
   fl1_put(b, 0, 6);
   fl1_put(b, 1, 16);                      /* floor type 1 */
   fl1_put(b, 1, 5);                       /* partitions */
   fl1_put(b, 0, 4);                       /* partition 0 uses class 0 */
   fl1_put(b, (unsigned) (ninterior - 1), 3);  /* class dimensions */
   fl1_put(b, 0, 2);                       /* subclasses */
   fl1_put(b, 3, 8);                       /* subclass book 2 (wire = id+1) */
   fl1_put(b, (unsigned) (multiplier - 1), 2);
   fl1_put(b, (unsigned) rangebits, 4);
   for (i = 0; i < ninterior; ++i)
      fl1_put(b, (unsigned) interior_x[i], (unsigned) rangebits);

   /* residues: one type-1 residue covering the whole half block */
   fl1_put(b, 0, 6);
   fl1_put(b, 1, 16);                      /* residue type 1 */
   fl1_put(b, 0, 24);                      /* begin */
   fl1_put(b, (unsigned) (1 << (block_log - 1)), 24); /* end = n/2 */
   fl1_put(b, 0, 24);                      /* part_size - 1 = 0 */
   fl1_put(b, 0, 6);                       /* classifications - 1 = 0 */
   fl1_put(b, 0, 8);                       /* classbook */
   fl1_put(b, 1, 3);                       /* cascade: pass 0 present */
   fl1_put(b, 0, 1);                       /* no high bits */
   fl1_put(b, 1, 8);                       /* pass-0 book 1 */

   /* mappings: one mapping, one submap, no coupling, mono mux 0 */
   fl1_put(b, 0, 6);
   fl1_put(b, 0, 16);                      /* mapping type 0 */
   fl1_put(b, 0, 1);                       /* no submap flag */
   fl1_put(b, 0, 1);                       /* no coupling */
   fl1_put(b, 0, 2);                       /* reserved */
   fl1_put(b, 0, 8);                       /* discarded submap config byte */
   fl1_put(b, 0, 8);                       /* floor 0 */
   fl1_put(b, 0, 8);                       /* residue 0 */

   /* modes: one fixed-block mode */
   fl1_put(b, 0, 6);
   fl1_put(b, 0, 1);                       /* blockflag 0 */
   fl1_put(b, 0, 16);                      /* window type */
   fl1_put(b, 0, 16);                      /* transform type */
   fl1_put(b, 0, 8);                       /* mapping 0 */

   fl1_put(b, 1, 1);                       /* framing bit */
}

static void fl1_put_id(fl1_bits *b, int block_log)
{
   fl1_magic(b, 1);
   fl1_put(b, 0, 32);                      /* vorbis version */
   fl1_put(b, 1, 8);                       /* channels */
   fl1_put(b, 8000, 32);                   /* sample rate */
   fl1_put(b, 0, 32); fl1_put(b, 0, 32); fl1_put(b, 0, 32);
   fl1_put(b, (unsigned) block_log | ((unsigned) block_log << 4), 8);
   fl1_put(b, 1, 8);                       /* framing */
}

static void fl1_put_comment(fl1_bits *b)
{
   fl1_magic(b, 3);
   fl1_put(b, 0, 32);                      /* vendor length */
   fl1_put(b, 0, 32);                      /* comment count */
}

/* Audio packet: floor present, explicit raw floor scalars + residue
 * symbols (residue book symbols are 2-bit, values 1 or 2). */
static void fl1_put_audio(fl1_bits *b, int range, const int *floor_raw,
                          int floor_count, const unsigned char *res_sym,
                          int n2)
{
   int endpoint_bits = fl1_ilog(range) - 1;
   int j;
   fl1_put(b, 0, 1);                       /* audio packet type */
   fl1_put(b, 1, 1);                       /* floor used */
   fl1_put(b, (unsigned) floor_raw[0], endpoint_bits);
   fl1_put(b, (unsigned) floor_raw[1], endpoint_bits);
   for (j = 2; j < floor_count; ++j)
      fl1_put(b, fl1_revbits((unsigned) floor_raw[j], 8), 8);
   /* residue type 1 with part_size 1: n2 partitions, each carrying a
    * 1-bit classbook symbol (always 0) followed by its 2-bit dim-1 value. */
   for (j = 0; j < n2; ++j) {
      fl1_put(b, 0, 1);
      fl1_put(b, fl1_revbits(res_sym[j], 2), 2);
   }
}

/* Floor-missing packet: unused floor means the channel is zero; no residue
 * is encoded for it. */
static void fl1_put_silent(fl1_bits *b)
{
   fl1_put(b, 0, 1);
   fl1_put(b, 0, 1);
}

/* ===================================================================== */
/* stream assembly                                                       */
/* ===================================================================== */

typedef struct
{
   unsigned char bytes[8192];
   unsigned size;
} fl1_ogg;

static void fl1_emit(fl1_ogg *ogg, const fl1_bits *p, unsigned seq,
                     unsigned flags, uint64_t granule)
{
   ogg->size += fl1_page(ogg->bytes + ogg->size, p, seq, flags, granule);
}

struct fl1_audio_desc
{
   int floor_missing;
   const int *floor_raw;
   const unsigned char *res_sym;
};

static unsigned fl1_build_stream(fl1_ogg *ogg, int block_log, int multiplier,
                                 int rangebits, const int *interior_x,
                                 int ninterior, int npackets,
                                 const struct fl1_audio_desc *packets)
{
   fl1_bits b;
   int n = 1 << block_log;
   int n2 = n >> 1;
   int floor_count = ninterior + 2;
   int k;
   memset(ogg, 0, sizeof(*ogg));

   memset(&b, 0, sizeof(b)); fl1_put_id(&b, block_log);
   fl1_emit(ogg, &b, 0, 2, 0);

   memset(&b, 0, sizeof(b)); fl1_put_comment(&b);
   fl1_emit(ogg, &b, 1, 0, 0);

   memset(&b, 0, sizeof(b));
   fl1_put_setup(&b, block_log, multiplier, rangebits, interior_x, ninterior);
   fl1_emit(ogg, &b, 2, 0, 0);

   for (k = 0; k < npackets; ++k) {
      unsigned flags = (k == npackets - 1) ? 4u : 0u;
      uint64_t granule = (uint64_t) k * (uint64_t) n2;
      memset(&b, 0, sizeof(b));
      if (packets[k].floor_missing)
         fl1_put_silent(&b);
      else
         fl1_put_audio(&b, multiplier == 1 ? 256 :
                       (multiplier == 2 ? 128 : (multiplier == 3 ? 86 : 64)),
                       packets[k].floor_raw, floor_count,
                       packets[k].res_sym, n2);
      fl1_emit(ogg, &b, (unsigned) (3 + k), flags, granule);
   }
   return ogg->size;
}

/* range_list per production floor1_multiplier */
static const int fl1_ranges[4] = { 256, 128, 86, 64 };

/* ===================================================================== */
/* independent transform / overlap model                                 */
/* ===================================================================== */

typedef struct
{
   int n, n2;
   float *cos_table;             /* n2 rows x n columns, float like output */
   float window[512];            /* n/2 entries (max block 1024 -> use 512) */
} fl1_transform;

static int fl1_transform_init(fl1_transform *t, int n)
{
   int i, j;
   t->n = n;
   t->n2 = n >> 1;
   t->cos_table = (float *) malloc((size_t) t->n2 * (size_t) n * sizeof(float));
   if (!t->cos_table) return 0;
   for (i = 0; i < n; ++i)
      for (j = 0; j < t->n2; ++j)
         t->cos_table[i * t->n2 + j] =
            (float) cos(M_PI / (2.0 * n) *
                        (2.0 * i + 1.0 + n / 2.0) * (2.0 * j + 1.0));
   for (i = 0; i < t->n2; ++i) {
      float s = (float) sin((i + 0.5) / t->n2 * 0.5 * M_PI);
      t->window[i] = (float) sin(0.5 * M_PI * s * s);
   }
   return 1;
}

static void fl1_transform_free(fl1_transform *t)
{
   free(t->cos_table);
   t->cos_table = NULL;
}

/* Direct-cosine IMDCT in float, the same precision class as the production
 * transform (the definition itself is independent; no stb helper is used). */
static void fl1_model_imdct(const fl1_transform *t, const double *spectrum,
                            float *time)
{
   int i, j;
   for (i = 0; i < t->n; ++i) {
      float acc = 0.0f;
      const float *row = t->cos_table + (size_t) i * t->n2;
      for (j = 0; j < t->n2; ++j)
         acc += (float) spectrum[j] * row[j];
      time[i] = acc;
   }
}

/* Full per-stream model: spectra -> windowed overlap PCM.  Output frame k
 * (k >= 1) has n/2 samples; packet 0 is pre-roll.  Mirroring
 * vorbis_finish_frame() for fixed blocks (left=0, right=n/2), the emitted
 * sample i is the current block's LEFT half time[i] windowed by w[i], plus
 * the previous block's stored RIGHT half time[n/2+i] windowed by the
 * time-reversed window.  Window/overlap math stays in float. */
static int fl1_model_pcm(const fl1_transform *t, int npackets,
                         double (*spectra)[512], double *pcm_out)
{
   float prev_tail[512];
   float time[1024];
   int k, i;
   memset(prev_tail, 0, sizeof(prev_tail));
   for (k = 0; k < npackets; ++k) {
      fl1_model_imdct(t, spectra[k], time);
      if (k > 0) {
         for (i = 0; i < t->n2; ++i)
            pcm_out[(k - 1) * t->n2 + i] =
               (double) (time[i] * t->window[i] +
                         prev_tail[i] * t->window[t->n2 - 1 - i]);
      }
      for (i = 0; i < t->n2; ++i) prev_tail[i] = time[t->n2 + i];
   }
   return (npackets - 1) * t->n2;
}

/* ===================================================================== */
/* points group                                                          */
/* ===================================================================== */

/*
 * Interdependent point set, multiplier 1 (range 256), rangebits 6.
 * Declaration order is deliberately not frequency order.
 *
 *  idx  X   target Y   wire value / role
 *   0    0     100      endpoint
 *   1   64     140      endpoint
 *   2   40     125      0  -> inactive, later reactivated as neighbor of 5
 *   3   10     103      odd negative delta against prediction 106
 *   4   25     120      even positive delta against prediction 114
 *   5   55     145      even positive delta, reactivates point 2
 *   6   18     225      raw 225 (== symmetric room 224 + 1): outer-branch
 *                         unpack past the symmetric range, recovered to 225
 *   7   60     140      descending non-divisible prediction (143 -> 140)
 */
static int points_recovery(void)
{
   static const int X[8] = { 0, 64, 40, 10, 25, 55, 18, 60 };
   static const int target[8] = { 100, 140, 125, 103, 120, 145, 225, 140 };
   int range = 256;
   int raw[8];
   fl1_point model[FL1_MAX_PTS];
   fl1_ogg ogg;
   struct fl1_audio_desc desc[1];
   unsigned char res_sym[128];
   stb_vorbis *f;
   int err = 0, j, low, high;

   /* raw scalars for interior points via the independent forward encoder,
    * which needs the recovered Y of earlier declaration-order points. */
   {
      fl1_point seed[FL1_MAX_PTS];
      for (j = 0; j < 8; ++j) { seed[j].x = X[j]; seed[j].active = 0; }
      seed[0].y = target[0]; seed[1].y = target[1];
      seed[0].active = seed[1].active = 1;
      raw[0] = target[0]; raw[1] = target[1];
      for (j = 2; j < 8; ++j) {
         int pred;
         fl1_model_neighbors(X, j, &low, &high);
         pred = fl1_model_predict(X[j], X[low], seed[low].y,
                                  X[high], seed[high].y);
         raw[j] = fl1_model_encode(target[j], pred, range);
         seed[j].y = fl1_model_unwrap(raw[j], pred, range);
         if (raw[j] != 0) {
            seed[j].active = 1;
            seed[low].active = 1;
            seed[high].active = 1;
         }
      }
      TASK_CHECK(raw[2] == 0);                 /* zero-delta point */
      TASK_CHECK(raw[6] >= 1);                 /* outer-range unpack used */
      for (j = 0; j < 8; ++j) TASK_CHECK(raw[j] >= 0 && raw[j] < 256);
   }

   fl1_model_recover(X, raw, 8, range, model);
   for (j = 0; j < 8; ++j)
      TASK_CHECK(model[j].y == target[j]);

   /* build a one-packet stream so open_memory's pumped frame decodes it */
   for (j = 0; j < 128; ++j) res_sym[j] = (unsigned char) (1 + (j & 1));
   desc[0].floor_missing = 0;
   desc[0].floor_raw = raw;
   desc[0].res_sym = res_sym;
   fl1_build_stream(&ogg, 8, 1, 6, X + 2, 6, 1, desc);

   f = stb_vorbis_open_memory(ogg.bytes, (int) ogg.size, &err, NULL);
   TASK_CHECK(f != NULL);
   if (!f) return 1;

   {
      Floor1 *g = &f->floor_config[0].floor1;
      TASK_CHECK(g->values == 8);
      TASK_CHECK(g->floor1_multiplier == 1);
      TASK_CHECK(g->rangebits == 6);
      for (j = 0; j < 8; ++j)
         TASK_CHECK(g->Xlist[j] == (unsigned short) X[j]);

      /* neighbor model vs precomputed production table, every interior pt */
      for (j = 2; j < 8; ++j) {
         fl1_model_neighbors(X, j, &low, &high);
         TASK_CHECK(g->neighbors[j][0] == (unsigned char) low);
         TASK_CHECK(g->neighbors[j][1] == (unsigned char) high);
      }

      /* sorted order vs model draw order */
      {
         int order[FL1_MAX_PTS];
         int n = fl1_model_draw_order(model, 8, order);
         int q;
         TASK_CHECK(n == 8);     /* point 2 reactivated, all draw */
         for (q = 0; q < n; ++q)
            TASK_CHECK(g->sorted_order[q] == (unsigned char) order[q]);
      }

      /* pumped first frame left packet-0 finalY in the decoder state */
      for (j = 0; j < 8; ++j) {
         TASK_CHECK(f->finalY[0][j] == (model[j].active ? model[j].y : -1));
      }
      TASK_CHECK(f->finalY[0][2] == model[2].y);  /* reactivated, not -1 */
   }
   stb_vorbis_close(f);
   return 0;
}

/* All four multiplier legal ranges, with values landing at each range's
 * edges so the endpoint bit widths and Y bounds are genuinely exercised. */
static int points_multipliers(void)
{
   static const int interior[3] = { 30, 8, 45 };   /* unsorted declaration */
   static const int X[5] = { 0, 64, 30, 8, 45 };
   int m;
   for (m = 1; m <= 4; ++m) {
      int range = fl1_ranges[m - 1];
      int endpoint_bits = fl1_ilog(range) - 1;
      int endpoint_max = (1 << endpoint_bits) - 1;   /* wire width limit */
      int target[5], raw[5];
      fl1_point model[FL1_MAX_PTS];
      fl1_ogg ogg;
      struct fl1_audio_desc desc[1];
      unsigned char res_sym[128];
      stb_vorbis *f;
      int err = 0, j, low, high;

      target[0] = range / 4;
      target[1] = endpoint_max - endpoint_max / 8;  /* high, still in range */
      TASK_CHECK(target[1] < range && target[1] > target[0]);
      for (j = 0; j < 128; ++j) res_sym[j] = (unsigned char) (1 + (j % 3 != 0));

      /* choose interior targets, then encode against live predictions */
      {
         fl1_point seed[FL1_MAX_PTS];
         for (j = 0; j < 5; ++j) { seed[j].x = X[j]; seed[j].active = 0; }
         seed[0].y = target[0]; seed[1].y = target[1];
         seed[0].active = seed[1].active = 1;
         raw[0] = target[0]; raw[1] = target[1];
         for (j = 2; j < 5; ++j) {
            int pred, want;
            fl1_model_neighbors(X, j, &low, &high);
            pred = fl1_model_predict(X[j], X[low], seed[low].y,
                                     X[high], seed[high].y);
            /* alternate sides of the prediction, stay strictly in range */
            want = pred + ((j & 1) ? 3 : -4);
            if (want < 1) want = pred + 2;
            if (want >= range) want = pred - 2;
            target[j] = want;
            raw[j] = fl1_model_encode(want, pred, range);
            seed[j].y = fl1_model_unwrap(raw[j], pred, range);
            seed[j].active = 1;
            seed[low].active = seed[high].active = 1;
            TASK_CHECK(raw[j] >= 0 && raw[j] < 256);
            TASK_CHECK(want >= 0 && want < range);
         }
      }

      fl1_model_recover(X, raw, 5, range, model);
      for (j = 0; j < 5; ++j)
         TASK_CHECK(model[j].y == target[j]);

      desc[0].floor_missing = 0;
      desc[0].floor_raw = raw;
      desc[0].res_sym = res_sym;
      fl1_build_stream(&ogg, 8, m, 6, interior, 3, 1, desc);
      f = stb_vorbis_open_memory(ogg.bytes, (int) ogg.size, &err, NULL);
      TASK_CHECK(f != NULL);
      if (!f) return 1;
      {
         Floor1 *g = &f->floor_config[0].floor1;
         TASK_CHECK(g->floor1_multiplier == (unsigned char) m);
         for (j = 0; j < 5; ++j) {
            TASK_CHECK(f->finalY[0][j] >= 0);
            TASK_CHECK(f->finalY[0][j] == model[j].y);
            TASK_CHECK(f->finalY[0][j] < range);
         }
      }
      stb_vorbis_close(f);
   }
   return 0;
}

/* ===================================================================== */
/* curve group                                                           */
/* ===================================================================== */

#define CURVE_N2 128

/* Active geometry: rising, falling with truncated division, rising again,
 * then a segment whose endpoint lies past n2 (clipped) and whose residual
 * crosses adx exactly (stair-step threshold).  Inactive point included. */
static int curve_segments(void)
{
   /* x must include endpoint 0; values are pre-multiplier Y */
   fl1_point pts[6];
   int order[FL1_MAX_PTS];
   int ymodel[CURVE_N2], yrat[CURVE_N2];
   float target[CURVE_N2 + 4];
   float residue[CURVE_N2 + 4];
   int16 finalY[6];
   int i, nactive;
   stb_vorbis *f;
   int err = 0;
   fl1_ogg ogg;
   struct fl1_audio_desc desc[1];
   static const int interior[1] = { 1 };
   int dummy_raw[3] = { 40, 200, 0 };
   unsigned char res_sym[128];

   pts[0].x = 0;   pts[0].y = 40;   pts[0].active = 1;
   pts[1].x = 12;  pts[1].y = 100;  pts[1].active = 1;
   pts[2].x = 30;  pts[2].y = 70;   pts[2].active = 1;   /* falling */
   pts[3].x = 40;  pts[3].y = 130;  pts[3].active = 1;
   pts[4].x = 150; pts[4].y = 200;  pts[4].active = 1;   /* clipped */
   pts[5].x = 20;  pts[5].y = 0;    pts[5].active = 0;   /* inactive */

   /* integer Y: both independent derivations must agree exactly */
   fl1_model_curve(pts, 6, 1, CURVE_N2, ymodel);
   {
      /* closed-form rational curve, built independently */
      int seg[][2] = { {0,40},{12,100},{30,70},{40,130},{150,200} };
      int q = 0, x;
      for (x = 0; x < CURVE_N2; ++x) {
         while (q < 4 && x >= seg[q+1][0]) ++q;
         if (x >= seg[4][0])
            yrat[x] = seg[4][1];
         else
            yrat[x] = fl1_line_rational(x, seg[q][0], seg[q][1],
                                        seg[q+1][0], seg[q+1][1]);
      }
      /* clipping: after x=128 the last value holds */
      for (x = 0; x < CURVE_N2; ++x) {
         TASK_CHECK(ymodel[x] == yrat[x]);
         TASK_CHECK(ymodel[x] >= 0 && ymodel[x] < 256);
      }
   }

   /* exact integer threshold crossings for the stair mutation: the
    * Bresenham residual lands exactly on adx, so '>=' vs '>' changes the
    * stepped Y.  The earliest such bin in this geometry is x=15 on the
    * falling segment (residual 12, adx 18: 12*(15-12) == 18*2); x=51 is a
    * later, independently-checkable crossing on the clipped long segment. */
   TASK_CHECK((12 * (15 - 12)) % 18 == 0);
   TASK_CHECK(fl1_line_step(15, 12, 100, 30, 70) ==
              fl1_line_rational(15, 12, 100, 30, 70));
   TASK_CHECK((70 * (51 - 40)) % 110 == 0);
   TASK_CHECK(70 * (51 - 40) > 0);
   /* descending truncated prediction: slope mutation flips bin 13 */
   TASK_CHECK(fl1_line_step(13, 12, 100, 30, 70) ==
              fl1_line_rational(13, 12, 100, 30, 70));
   TASK_CHECK(fl1_line_step(13, 12, 100, 30, 70) == 99);

   /* endpoint ownership: boundary bins belong to the following segment */
   TASK_CHECK(ymodel[12] == 100);
   TASK_CHECK(ymodel[30] == 70);
   TASK_CHECK(ymodel[40] == 130);

   /* non-constant, non-zero residue, plus front/back sentinels */
   for (i = 0; i < CURVE_N2 + 4; ++i) {
      residue[i] = 0.75f + 0.13f * (float) (i % 7) + 0.007f * (float) (i % 3);
      target[i] = residue[i];
   }
   target[CURVE_N2] = target[CURVE_N2 + 1] = 999.0f;     /* rear sentinels */

   /* need a live decoder for do_floor's f argument; open any valid stream */
   for (i = 0; i < 128; ++i) res_sym[i] = (unsigned char) (1 + (i & 1));
   desc[0].floor_missing = 0;
   desc[0].floor_raw = dummy_raw;
   desc[0].res_sym = res_sym;
   fl1_build_stream(&ogg, 8, 1, 6, interior, 1, 1, desc);
   f = stb_vorbis_open_memory(ogg.bytes, (int) ogg.size, &err, NULL);
   TASK_CHECK(f != NULL);
   if (!f) return 1;

   /* hand the production floor walker our geometry.  do_floor walks
    * sorted_order[1..values-1], so values must equal the active count and
    * the parallel arrays only contain the active points (pts[5] is the
    * deliberately inactive one). */
   {
      Floor1 *g = &f->floor_config[0].floor1;
      int j, a = 0;
      nactive = fl1_model_draw_order(pts, 6, order);
      g->values = (short) nactive;
      g->floor1_multiplier = 1;
      for (j = 0; j < nactive; ++j) {
         int idx = order[j];
         g->Xlist[j] = (uint16) pts[idx].x;
         finalY[j] = (int16) pts[idx].y;
         g->sorted_order[j] = (uint8) j;   /* already frequency-sorted */
         (void) a;
      }
   }

   {
      int ok = do_floor(f, &f->mapping[0], 0, 256, target, finalY, NULL);
      TASK_CHECK(ok);
   }

   for (i = 0; i < CURVE_N2; ++i) {
      double expected = (double) residue[i] *
                        (double) inverse_db_table[ymodel[i] & 255];
      TASK_CHECK(residue[i] != 0.0f);
      if (!fl1_near((double) target[i], expected, 2e-6)) {
         fprintf(stderr, "curve first mismatch bin=%d got=%.9g want=%.9g\n",
                 i, (double) target[i], expected);
         TASK_CHECK(0);
      }
   }
   /* sentinels untouched: do_floor never writes at or past n2 */
   TASK_CHECK(target[CURVE_N2] == 999.0f);
   TASK_CHECK(target[CURVE_N2 + 1] == 999.0f);
   stb_vorbis_close(f);
   return 0;
}

/* A purely flat-tail case: last active point before n2, verify the held
 * value propagates to the final bin (no clipping, no extra segment). */
static int curve_tailhold(void)
{
   fl1_point pts[3];
   int ymodel[CURVE_N2];
   int i;
   pts[0].x = 0;  pts[0].y = 123; pts[0].active = 1;
   pts[1].x = 20; pts[1].y = 150; pts[1].active = 1;
   pts[2].x = 60; pts[2].y = 0;   pts[2].active = 0;
   fl1_model_curve(pts, 3, 1, CURVE_N2, ymodel);
   TASK_CHECK(ymodel[0] == 123);
   TASK_CHECK(ymodel[20] == 150);
   for (i = 20; i < CURVE_N2; ++i) TASK_CHECK(ymodel[i] == 150);
   return 0;
}

/* ===================================================================== */
/* packets group                                                         */
/* ===================================================================== */

#define PK_NPACKETS 4          /* A active, B active, C missing, D active */

static int packets_run(int block_log)
{
   static const int interior[2] = { 40, 15 };      /* unsorted */
   static const int X[4] = { 0, 64, 40, 15 };
   const int range = 256;
   const int n = 1 << block_log;
   const int n2 = n >> 1;
   int rangebits = 6;
   int raw[PK_NPACKETS][4];
   int target_y[PK_NPACKETS][4];
   unsigned char res_sym[PK_NPACKETS][512];
   double spectra[PK_NPACKETS][512];
   double expected_pcm[3 * 512];
   fl1_point model[FL1_MAX_PTS];
   fl1_transform t;
   fl1_ogg ogg;
   struct fl1_audio_desc desc[PK_NPACKETS];
   stb_vorbis *f;
   int err = 0, k, j, low, high;
   int expected_samples;

   TASK_CHECK(fl1_transform_init(&t, n));
   if (!t.cos_table) return 1;

   for (k = 0; k < PK_NPACKETS; ++k) {
      fl1_point seed[FL1_MAX_PTS];
      for (j = 0; j < 4; ++j) { seed[j].x = X[j]; seed[j].active = 0; }
      /* distinct, non-flat endpoint/interior targets per active packet */
      target_y[k][0] = 90 + 12 * k;
      target_y[k][1] = 160 - 8 * k;
      seed[0].y = target_y[k][0];
      seed[1].y = target_y[k][1];
      seed[0].active = seed[1].active = 1;
      raw[k][0] = target_y[k][0];
      raw[k][1] = target_y[k][1];
      for (j = 2; j < 4; ++j) {
         int pred, want;
         fl1_model_neighbors(X, j, &low, &high);
         pred = fl1_model_predict(X[j], X[low], seed[low].y,
                                  X[high], seed[high].y);
         want = pred + ((j + k) & 1 ? 5 : -6);
         if (want < 1) want = pred + 3;
         if (want >= range) want = pred - 3;
         target_y[k][j] = want;
         raw[k][j] = fl1_model_encode(want, pred, range);
         seed[j].y = fl1_model_unwrap(raw[k][j], pred, range);
         seed[j].active = 1;
         seed[low].active = seed[high].active = 1;
         TASK_CHECK(raw[k][j] >= 0 && raw[k][j] < 256);
      }
      /* residue: every bin non-zero, pattern differs per packet */
      for (j = 0; j < n2; ++j)
         res_sym[k][j] = (unsigned char) (1 + ((j + k) & 1));
   }

   /* independent spectra for every packet, including the missing one */
   for (k = 0; k < PK_NPACKETS; ++k) {
      int ycurve[512];
      if (k == 2) {
         for (j = 0; j < n2; ++j) spectra[k][j] = 0.0;
         continue;
      }
      fl1_model_recover(X, raw[k], 4, range, model);
      fl1_model_curve(model, 4, 1, n2, ycurve);
      for (j = 0; j < n2; ++j) {
         double gain = (double) inverse_db_table[ycurve[j] & 255];
         double residue = (double) res_sym[k][j] * 0.5;  /* book mults */
         spectra[k][j] = gain * residue;
      }
   }
   /* floor-missing packet must have an all-zero current spectrum */
   for (j = 0; j < n2; ++j) TASK_CHECK(spectra[2][j] == 0.0);

   expected_samples = fl1_model_pcm(&t, PK_NPACKETS, spectra, expected_pcm);
   TASK_CHECK(expected_samples == (PK_NPACKETS - 1) * n2);

   for (k = 0; k < PK_NPACKETS; ++k) {
      desc[k].floor_missing = (k == 2);
      desc[k].floor_raw = raw[k];
      desc[k].res_sym = res_sym[k];
   }
   fl1_build_stream(&ogg, block_log, 1, rangebits, interior, 2,
                    PK_NPACKETS, desc);

   f = stb_vorbis_open_memory(ogg.bytes, (int) ogg.size, &err, NULL);
   TASK_CHECK(f != NULL);
   if (!f) { fl1_transform_free(&t); return 1; }
   {
      stb_vorbis_info info = stb_vorbis_get_info(f);
      TASK_CHECK(info.channels == 1);
      TASK_CHECK(info.sample_rate == 8000);
   }

   {
      /* capture every output frame; call index 1 is the floor-missing
       * packet's output (packet 0 is consumed as pre-roll inside open). */
      float prod_pcm[PK_NPACKETS - 1][512];
      int total = 0, call;
      for (call = 0; call < PK_NPACKETS - 1; ++call) {
         float **pcm = NULL;
         int channels = 0;
         int got = stb_vorbis_get_frame_float(f, &channels, &pcm);
         TASK_CHECK(got == n2);                 /* fixed-block output length */
         TASK_CHECK(channels == 1);
         if (got != n2) break;
         for (j = 0; j < n2; ++j)
            prod_pcm[call][j] = pcm[0][j];
         /* first call already accounts for packet-0 pre-roll */
         for (j = 0; j < n2; ++j) {
            double want = expected_pcm[call * n2 + j];
            if (!fl1_near((double) pcm[0][j], want, 2e-5)) {
               fprintf(stderr,
                  "packets n=%d first sample mismatch frame=%d idx=%d "
                  "got=%.9g want=%.9g\n",
                  n, call, j, (double) pcm[0][j], want);
               TASK_CHECK(0);
            }
         }
         total += got;
      }
      TASK_CHECK(total == expected_samples);

      /* the floor-missing packet's own spectrum is zero, yet its output
       * frame must carry packet B's overlap tail: never blanket-silent.
       * The sine window legitimately suppresses a couple of edge samples,
       * so require a strong majority plus substantial peak energy, and the
       * same property on the independent model frame. */
      {
         int nonzero = 0, j2;
         double peak = 0.0, model_peak = 0.0;
         for (j2 = 0; j2 < n2; ++j2) {
            double pv = fabs(prod_pcm[1][j2]);
            double mv = fabs(expected_pcm[n2 + j2]);
            if (pv > 1e-6) ++nonzero;
            if (pv > peak) peak = pv;
            if (mv > model_peak) model_peak = mv;
         }
         TASK_CHECK(nonzero >= (3 * n2) / 4);
         TASK_CHECK(peak > 1e-3);
         TASK_CHECK(model_peak > 1e-3);
      }
      TASK_CHECK(stb_vorbis_get_error(f) == VORBIS__no_error);
   }
   stb_vorbis_close(f);
   fl1_transform_free(&t);
   return 0;
}

static int packets_256(void) { return packets_run(8); }
static int packets_512(void) { return packets_run(9); }

/* ===================================================================== */
/* registration                                                          */
/* ===================================================================== */

const struct task_case task_cases[] = {
   { "points",  "recovery",    points_recovery },
   { "points",  "multipliers", points_multipliers },
   { "curve",   "segments",    curve_segments },
   { "curve",   "tailhold",    curve_tailhold },
   { "packets", "block256",    packets_256 },
   { "packets", "block512",    packets_512 },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
