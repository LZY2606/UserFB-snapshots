// File: rg_etc2.cpp - ETC2 RGB8 block packer/unpacker extending rg_etc1.
// See rg_etc1.h for the public API and the ZLIB license covering the original code.
//
// The 8-byte stream is the standard ETC2 RGB8 block. No private mode tags are
// stored: every emitted word is re-recognizable through the standard diff-bit
// and red/green/blue differential overflow priority (T, then H, then planar).
//
// Word convention used below: each 32-bit half of the block is byte-swapped on
// load, so bit positions match the reference decoder formulas (stream byte 0
// lands in bits 24..31, selector bytes 4..7 in bits 32..63).

#include "rg_etc1.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdexcept>
#include <vector>

namespace rg_etc1
{
   typedef unsigned char uint8;
   typedef unsigned short uint16;
   typedef unsigned int uint;
   typedef unsigned long long uint64;

   static const int cTHDistanceTable[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

   // ------------------------------------------------------------------
   // Word/field helpers (bit positions counted from the LSB of the word)
   // ------------------------------------------------------------------

   static inline uint64 load_word(const uint8* p)
   {
      uint64 w = 0;
      for (uint i = 0; i < 8; i++)
      {
         const uint bit = (i < 4) ? (24 - 8 * i) : (88 - 8 * i);
         w |= (uint64)p[i] << bit;
      }
      return w;
   }

   static inline void store_word(uint64 w, uint8* p)
   {
      for (uint i = 0; i < 8; i++)
      {
         const uint bit = (i < 4) ? (24 - 8 * i) : (88 - 8 * i);
         p[i] = static_cast<uint8>((w >> bit) & 0xFF);
      }
   }

   static inline uint get_bits(uint64 w, uint pos, uint n)
   {
      return static_cast<uint>((w >> pos) & ((1ULL << n) - 1));
   }

   static inline uint64 set_bits(uint64 w, uint pos, uint n, uint v)
   {
      const uint64 mask = ((1ULL << n) - 1) << pos;
      return (w & ~mask) | ((uint64)(v & ((1U << n) - 1)) << pos);
   }

   static inline int sign_extend3(uint v)
   {
      return (v & 4) ? (static_cast<int>(v) - 8) : static_cast<int>(v);
   }

   static inline int clamp_int(int v, int lo, int hi)
   {
      return v < lo ? lo : (v > hi ? hi : v);
   }

   static inline uint8 expand4(uint v) { return static_cast<uint8>((v << 4) | v); }
   static inline uint8 expand6(uint v) { return static_cast<uint8>((v << 2) | (v >> 4)); }
   static inline uint8 expand7(uint v) { return static_cast<uint8>((v << 1) | (v >> 6)); }

   // Floor division by 4; the planar formula specifies floor() before clamping.
   static inline int floor_div4(int v) { return v >= 0 ? (v >> 2) : -((-v + 3) >> 2); }

   // ------------------------------------------------------------------
   // Mode detection. diff=1 and red overflow -> T, green overflow -> H,
   // blue overflow -> planar; otherwise a legacy ETC1 block.
   // ------------------------------------------------------------------

   struct diff_view
   {
      bool diff;
      int r0, dr, r1;
      int g0, dg, g1;
      int b0, db, b1;
   };

   static diff_view read_diff_view(uint64 w)
   {
      diff_view d;
      d.diff = (get_bits(w, 1, 1) != 0);
      d.r0 = static_cast<int>(get_bits(w, 27, 5));
      d.g0 = static_cast<int>(get_bits(w, 19, 5));
      d.b0 = static_cast<int>(get_bits(w, 11, 5));
      d.dr = sign_extend3(get_bits(w, 24, 3));
      d.dg = sign_extend3(get_bits(w, 16, 3));
      d.db = sign_extend3(get_bits(w, 8, 3));
      d.r1 = d.r0 + d.dr;
      d.g1 = d.g0 + d.dg;
      d.b1 = d.b0 + d.db;
      return d;
   }

   static etc2_mode detect_mode(uint64 w)
   {
      const diff_view d = read_diff_view(w);
      if (!d.diff)
         return cETC2Individual;
      if (d.r1 < 0 || d.r1 > 31)
         return cETC2T;
      if (d.g1 < 0 || d.g1 > 31)
         return cETC2H;
      if (d.b1 < 0 || d.b1 > 31)
         return cETC2Planar;
      return cETC2Differential;
   }

   // ------------------------------------------------------------------
   // Selector helpers. The public RGBA array is row major (x,y); selector
   // bits are stored column major, k = x*4+y. The LSB plane is at word
   // bits 32..47 and the MSB plane at 48..63.
   // ------------------------------------------------------------------

   static inline uint th_selector_get(uint64 w, uint x, uint y)
   {
      const uint k = x * 4 + y;
      const uint lsb = get_bits(w, 32 + k, 1);
      const uint msb = get_bits(w, 48 + k, 1);
      return (msb << 1) | lsb;
   }

   static inline uint64 th_selector_set(uint64 w, uint x, uint y, uint s)
   {
      const uint k = x * 4 + y;
      w = set_bits(w, 32 + k, 1, s & 1);
      w = set_bits(w, 48 + k, 1, (s >> 1) & 1);
      return w;
   }

   static uint64 pack_selectors(uint64 w, const uint sel[16])
   {
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
            w = th_selector_set(w, x, y, sel[y * 4 + x]);
      return w;
   }

   // ------------------------------------------------------------------
   // Independent integer reconstruction of T, H and planar blocks.
   // ------------------------------------------------------------------

   struct rgb
   {
      int r, g, b;
      int& operator[] (uint c)
      {
         if (c == 0) return r;
         if (c == 1) return g;
         return b;
      }
      int operator[] (uint c) const
      {
         if (c == 0) return r;
         if (c == 1) return g;
         return b;
      }
   };

   static rgb clamp_rgb(int r, int g, int b)
   {
      rgb c;
      c.r = clamp_int(r, 0, 255);
      c.g = clamp_int(g, 0, 255);
      c.b = clamp_int(b, 0, 255);
      return c;
   }

   static void decode_t(uint64 w, rgb out[16])
   {
      const uint rh = get_bits(w, 27, 2);
      const uint rl = get_bits(w, 24, 2);
      const uint rA = (rh << 2) | rl;
      const rgb c0 = { expand4(rA), expand4(get_bits(w, 20, 4)), expand4(get_bits(w, 16, 4)) };
      const rgb base = { expand4(get_bits(w, 12, 4)), expand4(get_bits(w, 8, 4)), expand4(get_bits(w, 4, 4)) };

      const uint cw = (get_bits(w, 2, 2) << 1) | get_bits(w, 0, 1);
      const int d = cTHDistanceTable[cw];

      const rgb paints[4] =
      {
         c0,
         clamp_rgb(base.r + d, base.g + d, base.b + d),
         base,
         clamp_rgb(base.r - d, base.g - d, base.b - d),
      };

      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
            out[y * 4 + x] = paints[th_selector_get(w, x, y)];
   }

   static void decode_h(uint64 w, rgb out[16])
   {
      const uint r0 = get_bits(w, 27, 4);
      const uint g0 = get_bits(w, 20, 1) | (get_bits(w, 24, 3) << 1);
      const uint b0 = get_bits(w, 15, 3) | (get_bits(w, 19, 1) << 3);
      const uint r1 = get_bits(w, 11, 4);
      const uint g1 = get_bits(w, 7, 4);
      const uint b1 = get_bits(w, 3, 4);

      const rgb c0 = { expand4(r0), expand4(g0), expand4(b0) };
      const rgb c1 = { expand4(r1), expand4(g1), expand4(b1) };

      const uint packed0 = (r0 << 8) | (g0 << 4) | b0;
      const uint packed1 = (r1 << 8) | (g1 << 4) | b1;
      const uint cmp = (packed0 >= packed1) ? 1U : 0U;
      const uint cw = (get_bits(w, 2, 1) << 2) | (get_bits(w, 0, 1) << 1) | cmp;
      const int d = cTHDistanceTable[cw];

      const rgb paints[4] =
      {
         clamp_rgb(c0.r + d, c0.g + d, c0.b + d),
         clamp_rgb(c0.r - d, c0.g - d, c0.b - d),
         clamp_rgb(c1.r + d, c1.g + d, c1.b + d),
         clamp_rgb(c1.r - d, c1.g - d, c1.b - d),
      };

      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
            out[y * 4 + x] = paints[th_selector_get(w, x, y)];
   }

   static void decode_planar(uint64 w, rgb out[16])
   {
      // V anchor at parameter coord (0,4), H at (4,0), O at (0,0).
      const uint bv = get_bits(w, 32, 6), gv = get_bits(w, 38, 7), rv = get_bits(w, 45, 6);
      const uint bh = get_bits(w, 51, 6), gh = get_bits(w, 57, 7);
      const uint rh = get_bits(w, 0, 1) | (get_bits(w, 2, 5) << 1);

      const uint bo = get_bits(w, 7, 3) | (get_bits(w, 11, 2) << 3) | (get_bits(w, 16, 1) << 5);
      const uint go = get_bits(w, 17, 6) | (get_bits(w, 24, 1) << 6);
      const uint ro = get_bits(w, 25, 6);

      const rgb O = { expand6(ro), expand7(go), expand6(bo) };
      const rgb H = { expand6(rh), expand7(gh), expand6(bh) };
      const rgb V = { expand6(rv), expand7(gv), expand6(bv) };

      for (uint y = 0; y < 4; y++)
      {
         for (uint x = 0; x < 4; x++)
         {
            rgb p;
            p.r = clamp_int(floor_div4(x * (H.r - O.r) + y * (V.r - O.r) + 4 * O.r + 2), 0, 255);
            p.g = clamp_int(floor_div4(x * (H.g - O.g) + y * (V.g - O.g) + 4 * O.g + 2), 0, 255);
            p.b = clamp_int(floor_div4(x * (H.b - O.b) + y * (V.b - O.b) + 4 * O.b + 2), 0, 255);
            out[y * 4 + x] = p;
         }
      }
   }

   bool unpack_etc2_block(const void* pETC2_block, unsigned int* pDst_pixels_rgba,
      bool preserve_alpha, etc2_mode* pMode)
   {
      const uint8* pBytes = static_cast<const uint8*>(pETC2_block);
      const uint64 w = load_word(pBytes);
      const etc2_mode mode = detect_mode(w);
      if (pMode)
         *pMode = mode;

      if (mode == cETC2Individual || mode == cETC2Differential)
         return unpack_etc1_block(pETC2_block, pDst_pixels_rgba, preserve_alpha);

      rgb decoded[16];
      if (mode == cETC2T)
         decode_t(w, decoded);
      else if (mode == cETC2H)
         decode_h(w, decoded);
      else
         decode_planar(w, decoded);

      uint8* dst = reinterpret_cast<uint8*>(pDst_pixels_rgba);
      for (uint i = 0; i < 16; i++)
      {
         dst[i * 4 + 0] = static_cast<uint8>(decoded[i].r);
         dst[i * 4 + 1] = static_cast<uint8>(decoded[i].g);
         dst[i * 4 + 2] = static_cast<uint8>(decoded[i].b);
         if (!preserve_alpha)
            dst[i * 4 + 3] = 255;
      }
      return true;
   }

   // ------------------------------------------------------------------
   // Encoder internals
   // ------------------------------------------------------------------

   struct src_block
   {
      uint8 px[16][4]; // row major RGBA bytes
   };

   struct candidate
   {
      uint64 word;
      uint error;
      etc2_mode mode;
      candidate() : word(0), error(0xFFFFFFFFU), mode(cETC2Individual) {}
   };

   static inline uint sqr_diff(int a, int b) { const int d = a - b; return static_cast<uint>(d * d); }

   static uint rgb_error(const src_block& src, const rgb& dec, uint idx)
   {
      return sqr_diff(src.px[idx][0], dec.r) + sqr_diff(src.px[idx][1], dec.g) + sqr_diff(src.px[idx][2], dec.b);
   }

   // Distortion of an actual emitted word, decoded through the standard path.
   static uint measure_word_error(uint64 w, const src_block& src, uint valid_mask)
   {
      rgb decoded[16];
      const etc2_mode mode = detect_mode(w);
      if (mode == cETC2T)
         decode_t(w, decoded);
      else if (mode == cETC2H)
         decode_h(w, decoded);
      else if (mode == cETC2Planar)
         decode_planar(w, decoded);
      else
      {
         uint8 bytes[8];
         store_word(w, bytes);
         unsigned rgba[16];
         unpack_etc1_block(bytes, rgba, false);
         const uint8* p = reinterpret_cast<const uint8*>(rgba);
         for (uint i = 0; i < 16; i++)
            decoded[i] = { p[i * 4], p[i * 4 + 1], p[i * 4 + 2] };
      }

      uint total = 0;
      for (uint i = 0; i < 16; i++)
         if (valid_mask & (1U << i))
            total += rgb_error(src, decoded[i], i);
      return total;
   }

   static inline uint quant4(int v) { return static_cast<uint>(clamp_int((v * 15 + 127) / 255, 0, 15)); }
   static inline uint quant6(int v) { return static_cast<uint>(clamp_int((v * 63 + 127) / 255, 0, 63)); }
   static inline uint quant7(int v) { return static_cast<uint>(clamp_int((v * 127 + 127) / 255, 0, 127)); }

   // ------------------------- T mode -------------------------

   static uint64 build_t_word(uint rA, uint gA, uint bA, uint rB, uint gB, uint bB,
      uint cw, const uint sel[16])
   {
      uint64 w = 0;
      // Endpoint A: red is split into 2 high + 2 low bits around the delta field.
      w = set_bits(w, 27, 2, (rA >> 2) & 3);
      w = set_bits(w, 24, 2, rA & 3);
      w = set_bits(w, 20, 4, gA);
      w = set_bits(w, 16, 4, bA);
      w = set_bits(w, 12, 4, rB);
      w = set_bits(w, 8, 4, gB);
      w = set_bits(w, 4, 4, bB);
      w = set_bits(w, 2, 2, (cw >> 1) & 3);
      w = set_bits(w, 0, 1, cw & 1);
      w = set_bits(w, 1, 1, 1); // diff
      w = pack_selectors(w, sel);

      // Force the red differential to overflow while leaving the fields intact.
      const uint rh = rA >> 2, rl = rA & 3;
      if (rh + rl >= 4)
      {
         w = set_bits(w, 31, 1, 1);
         w = set_bits(w, 29, 2, 7); // detect1 = 111
         // detect2 (bit26) stays 0: positive overflow
      }
      else
      {
         w = set_bits(w, 26, 1, 1); // detect2 = 1: negative overflow
      }
      return w;
   }

   static void t_paints(uint rA, uint gA, uint bA, uint rB, uint gB, uint bB, uint cw, rgb paints[4])
   {
      const rgb c0 = { expand4(rA), expand4(gA), expand4(bA) };
      const rgb base = { expand4(rB), expand4(gB), expand4(bB) };
      const int d = cTHDistanceTable[cw];
      paints[0] = c0;
      paints[1] = clamp_rgb(base.r + d, base.g + d, base.b + d);
      paints[2] = base;
      paints[3] = clamp_rgb(base.r - d, base.g - d, base.b - d);
   }

   static uint eval_th_config(const src_block& src, uint valid_mask, const rgb paints[4],
      uint sel[16], uint best_so_far)
   {
      uint total = 0;
      for (uint i = 0; i < 16; i++)
      {
         if (!(valid_mask & (1U << i)))
         {
            sel[i] = 0;
            continue;
         }
         uint best_s = 0, best_e = rgb_error(src, paints[0], i);
         for (uint s = 1; s < 4; s++)
         {
            const uint e = rgb_error(src, paints[s], i);
            if (e < best_e) { best_e = e; best_s = s; }
         }
         sel[i] = best_s;
         total += best_e;
         if (total >= best_so_far)
            return total;
      }
      return total;
   }

   // Lloyd seed in 444 space over the valid pixels.
   static void kmeans_444(const src_block& src, uint valid_mask, uint seedA[3], uint seedB[3])
   {
      int cnt = 0, avg[3] = { 0, 0, 0 };
      for (uint i = 0; i < 16; i++)
         if (valid_mask & (1U << i))
         {
            avg[0] += src.px[i][0]; avg[1] += src.px[i][1]; avg[2] += src.px[i][2];
            cnt++;
         }
      if (!cnt) cnt = 1;
      rgb center = { avg[0] / cnt, avg[1] / cnt, avg[2] / cnt };

      uint far = 0; int far_d = -1;
      for (uint i = 0; i < 16; i++)
         if (valid_mask & (1U << i))
         {
            const int dd = sqr_diff(src.px[i][0], center.r) + sqr_diff(src.px[i][1], center.g) + sqr_diff(src.px[i][2], center.b);
            if (dd > far_d) { far_d = dd; far = i; }
         }

      rgb s0, s1;
      for (uint c = 0; c < 3; c++)
      {
         const int off = (static_cast<int>(src.px[far][c]) - center[c]) / 2;
         s0[c] = clamp_int(center[c] + off, 0, 255);
         s1[c] = clamp_int(center[c] - off, 0, 255);
      }

      for (uint iter = 0; iter < 10; iter++)
      {
         int sum[2][3] = { {0,0,0},{0,0,0} }, n[2] = { 0, 0 };
         for (uint i = 0; i < 16; i++)
            if (valid_mask & (1U << i))
            {
               const int d0 = sqr_diff(src.px[i][0], s0.r) + sqr_diff(src.px[i][1], s0.g) + sqr_diff(src.px[i][2], s0.b);
               const int d1 = sqr_diff(src.px[i][0], s1.r) + sqr_diff(src.px[i][1], s1.g) + sqr_diff(src.px[i][2], s1.b);
               const uint g = d0 <= d1 ? 0 : 1;
               for (uint c = 0; c < 3; c++) sum[g][c] += src.px[i][c];
               n[g]++;
            }
         rgb n0 = s0, n1 = s1;
         if (n[0]) for (uint c = 0; c < 3; c++) n0[c] = sum[0][c] / n[0];
         if (n[1]) for (uint c = 0; c < 3; c++) n1[c] = sum[1][c] / n[1];
         if (n0.r == s0.r && n0.g == s0.g && n0.b == s0.b &&
            n1.r == s1.r && n1.g == s1.g && n1.b == s1.b)
            break;
         s0 = n0; s1 = n1;
      }

      seedA[0] = quant4(s0.r); seedA[1] = quant4(s0.g); seedA[2] = quant4(s0.b);
      seedB[0] = quant4(s1.r); seedB[1] = quant4(s1.g); seedB[2] = quant4(s1.b);
   }

   static void try_t_candidate(const src_block& src, uint valid_mask, candidate& best,
      uint rA, uint gA, uint bA, uint rB, uint gB, uint bB, uint cw)
   {
      rgb paints[4];
      t_paints(rA, gA, bA, rB, gB, bB, cw, paints);
      uint sel[16];
      const uint e = eval_th_config(src, valid_mask, paints, sel, best.error);
      if (e >= best.error)
         return;
      const uint64 w = build_t_word(rA, gA, bA, rB, gB, bB, cw, sel);
      if (detect_mode(w) != cETC2T)
         return;
      const uint actual = measure_word_error(w, src, valid_mask);
      if (actual < best.error)
      {
         best.error = actual;
         best.word = w;
         best.mode = cETC2T;
      }
   }

   static candidate search_t(const src_block& src, uint valid_mask, etc1_quality quality)
   {
      uint qA[3], qB[3];
      kmeans_444(src, valid_mask, qA, qB);

      const int reach = quality == cLowQuality ? 1 : 2;
      const uint sweeps = quality == cLowQuality ? 2U : (quality == cMediumQuality ? 3U : 4U);
      candidate best;

      uint cur[2][3] = { { qA[0], qA[1], qA[2] }, { qB[0], qB[1], qB[2] } };

      // Initial evaluation at the k-means seeds (both endpoint orders).
      for (uint swap = 0; swap < 2; swap++)
      {
         const uint* a = swap ? cur[1] : cur[0];
         const uint* b = swap ? cur[0] : cur[1];
         for (uint cw = 0; cw < 8; cw++)
            try_t_candidate(src, valid_mask, best, a[0], a[1], a[2], b[0], b[1], b[2], cw);
      }

      // Coordinate descent on the six endpoint components, jointly with the
      // distance index and the per-pixel selectors.
      for (uint sweep = 0; sweep < sweeps; sweep++)
      {
         bool moved = false;
         for (uint ep = 0; ep < 2; ep++)
         {
            for (uint c = 0; c < 3; c++)
            {
               const uint center = cur[ep][c];
               for (int step = -reach; step <= reach; step++)
               {
                  if (!step) continue;
                  const int trial = static_cast<int>(center) + step;
                  if (trial < 0 || trial > 15)
                     continue;
                  uint trial_ep[3] = { cur[ep][0], cur[ep][1], cur[ep][2] };
                  trial_ep[c] = static_cast<uint>(trial);
                  const uint* a = ep == 0 ? trial_ep : cur[1 - ep];
                  const uint* b = ep == 1 ? trial_ep : cur[1 - ep];
                  const uint before = best.error;
                  for (uint cw = 0; cw < 8; cw++)
                     try_t_candidate(src, valid_mask, best, a[0], a[1], a[2], b[0], b[1], b[2], cw);
                  if (best.error < before)
                  {
                     cur[ep][c] = static_cast<uint>(trial);
                     moved = true;
                  }
               }
            }
         }
         if (!moved)
            break;
      }
      return best;
   }

   // ------------------------- H mode -------------------------

   static uint64 build_h_word(uint r0, uint g0, uint b0, uint r1, uint g1, uint b1,
      uint cw, const uint sel_in[16])
   {
      const uint packed0 = (r0 << 8) | (g0 << 4) | b0;
      const uint packed1 = (r1 << 8) | (g1 << 4) | b1;
      const bool odd = (cw & 1) != 0;
      // Stored endpoint order contributes the LSB of the distance index.
      const bool swap_colors = ((packed0 < packed1) ^ !odd);

      uint s0[3] = { r0, g0, b0 }, s1[3] = { r1, g1, b1 };
      uint sel[16];
      memcpy(sel, sel_in, sizeof(sel));
      if (swap_colors)
      {
         for (uint c = 0; c < 3; c++) { uint t = s0[c]; s0[c] = s1[c]; s1[c] = t; }
         // Swapping endpoints maps selector s -> s^2, i.e. flip the MSB plane.
         for (uint i = 0; i < 16; i++) sel[i] ^= 2;
      }

      uint64 w = 0;
      w = set_bits(w, 27, 4, s0[0]);
      w = set_bits(w, 24, 3, (s0[1] >> 1) & 7);
      w = set_bits(w, 20, 1, s0[1] & 1);
      w = set_bits(w, 19, 1, (s0[2] >> 3) & 1);
      w = set_bits(w, 15, 3, s0[2] & 7);
      w = set_bits(w, 11, 4, s1[0]);
      w = set_bits(w, 7, 4, s1[1]);
      w = set_bits(w, 3, 4, s1[2]);
      w = set_bits(w, 2, 1, (cw >> 2) & 1);
      w = set_bits(w, 0, 1, (cw >> 1) & 1);
      w = set_bits(w, 1, 1, 1); // diff
      w = pack_selectors(w, sel);

      // Red differential must stay in range; green differential must overflow.
      diff_view dv = read_diff_view(w);
      if (dv.r1 < 0 || dv.r1 > 31)
         w = set_bits(w, 31, 1, 1); // detect1
      dv = read_diff_view(w);
      if (dv.g1 >= 4)
      {
         w = set_bits(w, 23, 1, 1);
         w = set_bits(w, 21, 3, 7); // detect2 = 111, detect3 = 0
      }
      else
      {
         w = set_bits(w, 18, 1, 1); // detect3 = 1
      }
      return w;
   }

   static void h_paints(uint r0, uint g0, uint b0, uint r1, uint g1, uint b1, uint cw, rgb paints[4])
   {
      const rgb c0 = { expand4(r0), expand4(g0), expand4(b0) };
      const rgb c1 = { expand4(r1), expand4(g1), expand4(b1) };
      const int d = cTHDistanceTable[cw];
      paints[0] = clamp_rgb(c0.r + d, c0.g + d, c0.b + d);
      paints[1] = clamp_rgb(c0.r - d, c0.g - d, c0.b - d);
      paints[2] = clamp_rgb(c1.r + d, c1.g + d, c1.b + d);
      paints[3] = clamp_rgb(c1.r - d, c1.g - d, c1.b - d);
   }

   static void try_h_candidate(const src_block& src, uint valid_mask, candidate& best,
      uint r0, uint g0, uint b0, uint r1, uint g1, uint b1, uint cw)
   {
      rgb paints[4];
      h_paints(r0, g0, b0, r1, g1, b1, cw, paints);
      uint sel[16];
      const uint e = eval_th_config(src, valid_mask, paints, sel, best.error);
      if (e >= best.error)
         return;
      const uint64 w = build_h_word(r0, g0, b0, r1, g1, b1, cw, sel);
      if (detect_mode(w) != cETC2H)
         return;
      const uint actual = measure_word_error(w, src, valid_mask);
      if (actual < best.error)
      {
         best.error = actual;
         best.word = w;
         best.mode = cETC2H;
      }
   }

   static candidate search_h(const src_block& src, uint valid_mask, etc1_quality quality)
   {
      uint q0[3], q1[3];
      kmeans_444(src, valid_mask, q0, q1);

      const int reach = quality == cLowQuality ? 1 : 2;
      const uint sweeps = quality == cLowQuality ? 2U : (quality == cMediumQuality ? 3U : 4U);
      candidate best;

      uint cur[2][3] = { { q0[0], q0[1], q0[2] }, { q1[0], q1[1], q1[2] } };

      for (uint swap = 0; swap < 2; swap++)
      {
         const uint* a = swap ? cur[1] : cur[0];
         const uint* b = swap ? cur[0] : cur[1];
         for (uint cw = 0; cw < 8; cw++)
            try_h_candidate(src, valid_mask, best, a[0], a[1], a[2], b[0], b[1], b[2], cw);
      }

      for (uint sweep = 0; sweep < sweeps; sweep++)
      {
         bool moved = false;
         for (uint ep = 0; ep < 2; ep++)
         {
            for (uint c = 0; c < 3; c++)
            {
               const uint center = cur[ep][c];
               for (int step = -reach; step <= reach; step++)
               {
                  if (!step) continue;
                  const int trial = static_cast<int>(center) + step;
                  if (trial < 0 || trial > 15)
                     continue;
                  uint trial_ep[3] = { cur[ep][0], cur[ep][1], cur[ep][2] };
                  trial_ep[c] = static_cast<uint>(trial);
                  const uint* a = ep == 0 ? trial_ep : cur[1 - ep];
                  const uint* b = ep == 1 ? trial_ep : cur[1 - ep];
                  const uint before = best.error;
                  for (uint cw = 0; cw < 8; cw++)
                     try_h_candidate(src, valid_mask, best, a[0], a[1], a[2], b[0], b[1], b[2], cw);
                  if (best.error < before)
                  {
                     cur[ep][c] = static_cast<uint>(trial);
                     moved = true;
                  }
               }
            }
         }
         if (!moved)
            break;
      }
      return best;
   }

   // ------------------------- Planar mode -------------------------

   struct anchors
   {
      uint o[3], h[3], v[3]; // R/B in 0..63, G in 0..127
   };

   static void decode_planar_anchors(const anchors& a, rgb out[16])
   {
      rgb O, H, V;
      for (uint c = 0; c < 3; c++)
      {
         O[c] = c == 1 ? expand7(a.o[c]) : expand6(a.o[c]);
         H[c] = c == 1 ? expand7(a.h[c]) : expand6(a.h[c]);
         V[c] = c == 1 ? expand7(a.v[c]) : expand6(a.v[c]);
      }
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            rgb p;
            p.r = clamp_int(floor_div4(x * (H.r - O.r) + y * (V.r - O.r) + 4 * O.r + 2), 0, 255);
            p.g = clamp_int(floor_div4(x * (H.g - O.g) + y * (V.g - O.g) + 4 * O.g + 2), 0, 255);
            p.b = clamp_int(floor_div4(x * (H.b - O.b) + y * (V.b - O.b) + 4 * O.b + 2), 0, 255);
            out[y * 4 + x] = p;
         }
   }

   static uint planar_error(const src_block& src, uint valid_mask, const anchors& a, rgb decoded[16])
   {
      decode_planar_anchors(a, decoded);
      uint total = 0;
      for (uint i = 0; i < 16; i++)
         if (valid_mask & (1U << i))
            total += rgb_error(src, decoded[i], i);
      return total;
   }

   static uint64 build_planar_word(const anchors& a)
   {
      uint64 w = 0;
      // O anchor
      w = set_bits(w, 25, 6, a.o[0]);
      w = set_bits(w, 24, 1, (a.o[1] >> 6) & 1);
      w = set_bits(w, 17, 6, a.o[1] & 63);
      w = set_bits(w, 16, 1, (a.o[2] >> 5) & 1);
      w = set_bits(w, 11, 2, (a.o[2] >> 3) & 3);
      w = set_bits(w, 7, 3, a.o[2] & 7);
      // H anchor
      w = set_bits(w, 2, 5, (a.h[0] >> 1) & 31);
      w = set_bits(w, 0, 1, a.h[0] & 1);
      w = set_bits(w, 57, 7, a.h[1]);
      w = set_bits(w, 51, 6, a.h[2]);
      // V anchor
      w = set_bits(w, 45, 6, a.v[0]);
      w = set_bits(w, 38, 7, a.v[1]);
      w = set_bits(w, 32, 6, a.v[2]);
      w = set_bits(w, 1, 1, 1); // diff

      // Keep red/green differential in range, force the blue differential out.
      diff_view dv = read_diff_view(w);
      if (dv.r1 < 0 || dv.r1 > 31)
         w = set_bits(w, 31, 1, 1); // detect1
      dv = read_diff_view(w);
      if (dv.g1 < 0 || dv.g1 > 31)
         w = set_bits(w, 23, 1, 1); // detect2
      dv = read_diff_view(w);
      if (dv.b1 >= 4)
      {
         w = set_bits(w, 13, 3, 7); // detect3 = 111
         // detect4 (bit10) stays 0: positive overflow
      }
      else
      {
         w = set_bits(w, 10, 1, 1); // detect4 = 1: negative overflow
      }
      return w;
   }

   static candidate search_planar(const src_block& src, uint valid_mask, etc1_quality quality)
   {
      candidate best;
      anchors a;

      // Weighted least-squares plane fit z = px*x + py*y + c0 per channel.
      for (uint c = 0; c < 3; c++)
      {
         double sx = 0, sy = 0, sz = 0, sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
         uint n = 0;
         for (uint y = 0; y < 4; y++)
            for (uint x = 0; x < 4; x++)
            {
               const uint i = y * 4 + x;
               if (!(valid_mask & (1U << i)))
                  continue;
               const double z = src.px[i][c];
               sx += x; sy += y; sz += z; sxx += x * x; syy += y * y; sxy += x * y;
               sxz += x * z; syz += y * z; n++;
            }
         if (!n) n = 1;
         // Normal equations, solved by Cramer's rule.
         const double A = n * sxx - sx * sx;
         const double B = n * sxy - sx * sy;
         const double C = n * syy - sy * sy;
         const double det = A * C - B * B;
         double px = 0, py = 0, c0 = sz / n;
         if (fabs(det) > 1e-9)
         {
            const double rx = n * sxz - sx * sz;
            const double ry = n * syz - sy * sz;
            px = (rx * C - ry * B) / det;
            py = (A * ry - B * rx) / det;
            c0 = (sz - px * sx - py * sy) / n;
         }
         const int vO = clamp_int(static_cast<int>(lround(c0)), 0, 255);
         const int vH = clamp_int(static_cast<int>(lround(c0 + 4 * px)), 0, 255);
         const int vV = clamp_int(static_cast<int>(lround(c0 + 4 * py)), 0, 255);
         a.o[c] = c == 1 ? quant7(vO) : quant6(vO);
         a.h[c] = c == 1 ? quant7(vH) : quant6(vH);
         a.v[c] = c == 1 ? quant7(vV) : quant6(vV);
      }

      rgb decoded[16];
      uint cur_error = planar_error(src, valid_mask, a, decoded);

      // Coordinate descent over the quantized anchors; error is always measured
      // after full integer reconstruction (never on the unfitted endpoints).
      const int rounds = quality == cLowQuality ? 2 : (quality == cMediumQuality ? 3 : 4);
      const int reach = quality == cLowQuality ? 1 : 2;
      for (int round = 0; round < rounds; round++)
      {
         bool improved = false;
         for (uint which = 0; which < 3; which++)
         {
            uint* q = which == 0 ? a.o : (which == 1 ? a.h : a.v);
            for (uint c = 0; c < 3; c++)
            {
               const uint maxv = c == 1 ? 127U : 63U;
               const uint saved = q[c];
               for (int step = -reach; step <= reach; step++)
               {
                  if (!step) continue;
                  const int trial = static_cast<int>(saved) + step;
                  if (trial < 0 || trial > static_cast<int>(maxv))
                     continue;
                  q[c] = static_cast<uint>(trial);
                  const uint e = planar_error(src, valid_mask, a, decoded);
                  if (e < cur_error)
                  {
                     cur_error = e;
                     improved = true;
                  }
                  else
                     q[c] = saved;
               }
            }
         }
         if (!improved)
            break;
      }

      const uint64 w = build_planar_word(a);
      if (detect_mode(w) == cETC2Planar)
      {
         best.word = w;
         best.mode = cETC2Planar;
         best.error = measure_word_error(w, src, valid_mask);
      }
      return best;
   }

   // ------------------------- Block entry point -------------------------

   namespace
   {
      struct etc1_init_guard
      {
         etc1_init_guard() { pack_etc1_block_init(); }
      };
   }

   unsigned int pack_etc2_block(void* pETC2_block, const unsigned int* pSrc_pixels_rgba,
      const etc2_options& options, unsigned int valid_mask)
   {
      valid_mask &= 0xFFFFU;
      if (!valid_mask)
         throw std::invalid_argument("pack_etc2_block: valid_mask must have at least one of the low 16 bits set");
      // Validate through an integer so an out-of-range enum value cannot turn
      // the range check itself into undefined behavior.
      int quality_value;
      memcpy(&quality_value, &options.m_quality, sizeof(quality_value));
      if (quality_value < static_cast<int>(cLowQuality) || quality_value > static_cast<int>(cHighQuality))
         throw std::invalid_argument("pack_etc2_block: invalid quality");

      static const etc1_init_guard guard;

      src_block src;
      for (uint i = 0; i < 16; i++)
      {
         const uint8* p = reinterpret_cast<const uint8*>(&pSrc_pixels_rgba[i]);
         src.px[i][0] = p[0];
         src.px[i][1] = p[1];
         src.px[i][2] = p[2];
         src.px[i][3] = p[3];
      }

      candidate best;

      // Legacy ETC1 individual + differential candidate, built by the original packer.
      {
         unsigned etc1_src[16];
         uint8* ep = reinterpret_cast<uint8*>(etc1_src);
         for (uint i = 0; i < 16; i++)
         {
            ep[i * 4 + 0] = src.px[i][0];
            ep[i * 4 + 1] = src.px[i][1];
            ep[i * 4 + 2] = src.px[i][2];
            ep[i * 4 + 3] = 255;
         }
         etc1_pack_params params;
         params.m_quality = options.m_quality;
         params.m_dithering = false;
         uint8 bytes[8];
         pack_etc1_block(bytes, etc1_src, params);
         const uint64 w = load_word(bytes);
         candidate etc1_cand;
         etc1_cand.word = w;
         etc1_cand.mode = detect_mode(w);
         etc1_cand.error = measure_word_error(w, src, valid_mask);
         best = etc1_cand;
      }

      if (options.m_allow_t)
      {
         const candidate c = search_t(src, valid_mask, options.m_quality);
         if (c.error < best.error) best = c;
      }
      if (options.m_allow_h)
      {
         const candidate c = search_h(src, valid_mask, options.m_quality);
         if (c.error < best.error) best = c;
      }
      if (options.m_allow_planar)
      {
         const candidate c = search_planar(src, valid_mask, options.m_quality);
         if (c.error < best.error) best = c;
      }

      uint8* dst = static_cast<uint8*>(pETC2_block);
      store_word(best.word, dst);
      return best.error;
   }

   // ------------------------- Whole image API -------------------------

   std::vector<unsigned char> pack_etc2_image(const unsigned char* pSrc_rgba,
      unsigned int width, unsigned int height, size_t stride, const etc2_options& options)
   {
      if (!width || !height)
         throw std::invalid_argument("pack_etc2_image: width and height must be non-zero");
      if (!pSrc_rgba)
         throw std::invalid_argument("pack_etc2_image: null source buffer");
      if (static_cast<size_t>(width) > SIZE_MAX / 4U || static_cast<size_t>(width) * 4U > stride)
         throw std::invalid_argument("pack_etc2_image: stride must be at least width*4");
      // Maximum readable offset is (height-1)*stride + width*4 <= height*stride.
      if (static_cast<size_t>(height) > SIZE_MAX / stride)
         throw std::invalid_argument("pack_etc2_image: source size overflows");

      const size_t blocks_x = (static_cast<size_t>(width) + 3U) / 4U;
      const size_t blocks_y = (static_cast<size_t>(height) + 3U) / 4U;
      if (blocks_x > SIZE_MAX / 8U || blocks_y > SIZE_MAX / (blocks_x * 8U))
         throw std::invalid_argument("pack_etc2_image: block count overflow");
      const size_t total_bytes = blocks_x * blocks_y * 8U;

      std::vector<unsigned char> out(total_bytes);

      for (size_t by = 0; by < blocks_y; by++)
      {
         for (size_t bx = 0; bx < blocks_x; bx++)
         {
            unsigned rgba[16];
            unsigned valid_mask = 0;
            for (unsigned int yy = 0; yy < 4; yy++)
            {
               // Edge replication for the partial bottom/right border.
               const size_t sy = by * 4 + yy;
               const size_t src_y = sy < height ? sy : static_cast<size_t>(height) - 1U;
               for (unsigned int xx = 0; xx < 4; xx++)
               {
                  const size_t sx = bx * 4 + xx;
                  const size_t src_x = sx < width ? sx : static_cast<size_t>(width) - 1U;
                  const uint8* sp = pSrc_rgba + src_y * stride + src_x * 4U;
                  uint8* dp = reinterpret_cast<uint8*>(&rgba[yy * 4 + xx]);
                  dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                  if (sx < width && sy < height)
                     valid_mask |= 1U << (yy * 4 + xx);
               }
            }
            pack_etc2_block(&out[(by * blocks_x + bx) * 8U],
               rgba, options, valid_mask);
         }
      }
      return out;
   }

} // namespace rg_etc1
