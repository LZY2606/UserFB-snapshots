// File: task_tests/target.cpp - ETC2 RGB8 task tests.
//
// Four groups registered through task_support/registry.hpp:
//   decode       - independent bit-level reference, mode trigger boundaries,
//                  H endpoint ordering, alpha handling
//   search       - reproducible per-mode textures, selected mode, true
//                  post-reconstruction distortion, continuous-vs-quantized
//                  counterexamples, monotonicity when enabling modes
//   images       - 1x1, 5x7, row sentinels (stride padding), edge masks,
//                  small PPM output
//   compatibility- legacy ETC1 byte identity, three qualities, deterministic
//                  output, multithreaded interleaved encoding
//
// The reference decoder below is written from the raw bit positions and uses
// only integer reconstruction; it does not call into the production encoder or
// decoder, so an implementation that only round-trips itself cannot pass.

#include "task_support/registry.hpp"
#include "rg_etc1.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rg_etc1;

namespace
{
   typedef unsigned int uint;
   typedef unsigned long long uint64;
   typedef unsigned char uint8;

   const int kTHDist[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };
   const int kEtc1Tabs[8][4] =
   {
      { -8, -2, 2, 8 }, { -17, -5, 5, 17 }, { -29, -9, 9, 29 }, { -42, -13, 13, 42 },
      { -60, -18, 18, 60 }, { -80, -24, 24, 80 }, { -106, -33, 33, 106 }, { -183, -47, 47, 183 }
   };
   const int kEtc1Perm[4] = { 2, 3, 1, 0 };

   struct RefRGB { int r, g, b; };

   uint ref_bits(uint64 w, uint pos, uint n)
   {
      return (uint)((w >> pos) & ((1ULL << n) - 1));
   }

   uint64 ref_put(uint64 w, uint pos, uint n, uint v)
   {
      const uint64 mask = ((1ULL << n) - 1) << pos;
      return (w & ~mask) | ((uint64)(v & ((1U << n) - 1)) << pos);
   }

   int ref_sx3(uint v) { return (v & 4) ? (int)v - 8 : (int)v; }
   int ref_cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
   int ref_ex4(uint v) { return (v << 4) | v; }
   int ref_ex5(uint v) { return (v << 3) | (v >> 2); }
   int ref_ex6(uint v) { return (v << 2) | (v >> 4); }
   int ref_ex7(uint v) { return (v << 1) | (v >> 6); }
   int ref_floor4(int v) { return v >= 0 ? (v >> 2) : -((-v + 3) >> 2); }

   enum RefMode { REF_IND, REF_DIFF, REF_T, REF_H, REF_PLANAR };

   RefMode ref_detect(uint64 w)
   {
      if (!ref_bits(w, 1, 1))
         return REF_IND;
      const int r1 = (int)ref_bits(w, 27, 5) + ref_sx3(ref_bits(w, 24, 3));
      const int g1 = (int)ref_bits(w, 19, 5) + ref_sx3(ref_bits(w, 16, 3));
      const int b1 = (int)ref_bits(w, 11, 5) + ref_sx3(ref_bits(w, 8, 3));
      if (r1 < 0 || r1 > 31) return REF_T;
      if (g1 < 0 || g1 > 31) return REF_H;
      if (b1 < 0 || b1 > 31) return REF_PLANAR;
      return REF_DIFF;
   }

   uint ref_selector(uint64 w, uint x, uint y)
   {
      const uint k = x * 4 + y;
      return (ref_bits(w, 48 + k, 1) << 1) | ref_bits(w, 32 + k, 1);
   }

   // Independent integer reconstruction straight from the bit positions.
   void ref_decode(uint64 w, RefRGB out[16])
   {
      const RefMode mode = ref_detect(w);
      if (mode == REF_T)
      {
         const uint rA = (ref_bits(w, 27, 2) << 2) | ref_bits(w, 24, 2);
         const RefRGB c0 = { ref_ex4(rA), ref_ex4(ref_bits(w, 20, 4)), ref_ex4(ref_bits(w, 16, 4)) };
         const RefRGB base = { ref_ex4(ref_bits(w, 12, 4)), ref_ex4(ref_bits(w, 8, 4)), ref_ex4(ref_bits(w, 4, 4)) };
         const uint cw = (ref_bits(w, 2, 2) << 1) | ref_bits(w, 0, 1);
         const int d = kTHDist[cw];
         const RefRGB paints[4] =
         {
            c0,
            { ref_cl(base.r + d), ref_cl(base.g + d), ref_cl(base.b + d) },
            base,
            { ref_cl(base.r - d), ref_cl(base.g - d), ref_cl(base.b - d) }
         };
         for (uint y = 0; y < 4; y++)
            for (uint x = 0; x < 4; x++)
               out[y * 4 + x] = paints[ref_selector(w, x, y)];
         return;
      }
      if (mode == REF_H)
      {
         const uint r0 = ref_bits(w, 27, 4);
         const uint g0 = ref_bits(w, 20, 1) | (ref_bits(w, 24, 3) << 1);
         const uint b0 = ref_bits(w, 15, 3) | (ref_bits(w, 19, 1) << 3);
         const uint r1 = ref_bits(w, 11, 4);
         const uint g1 = ref_bits(w, 7, 4);
         const uint b1 = ref_bits(w, 3, 4);
         const RefRGB c0 = { ref_ex4(r0), ref_ex4(g0), ref_ex4(b0) };
         const RefRGB c1 = { ref_ex4(r1), ref_ex4(g1), ref_ex4(b1) };
         const uint cmp = ((r0 << 8) | (g0 << 4) | b0) >= ((r1 << 8) | (g1 << 4) | b1);
         const uint cw = (ref_bits(w, 2, 1) << 2) | (ref_bits(w, 0, 1) << 1) | cmp;
         const int d = kTHDist[cw];
         const RefRGB paints[4] =
         {
            { ref_cl(c0.r + d), ref_cl(c0.g + d), ref_cl(c0.b + d) },
            { ref_cl(c0.r - d), ref_cl(c0.g - d), ref_cl(c0.b - d) },
            { ref_cl(c1.r + d), ref_cl(c1.g + d), ref_cl(c1.b + d) },
            { ref_cl(c1.r - d), ref_cl(c1.g - d), ref_cl(c1.b - d) }
         };
         for (uint y = 0; y < 4; y++)
            for (uint x = 0; x < 4; x++)
               out[y * 4 + x] = paints[ref_selector(w, x, y)];
         return;
      }
      if (mode == REF_PLANAR)
      {
         const RefRGB O = { ref_ex6(ref_bits(w, 25, 6)),
            ref_ex7(ref_bits(w, 17, 6) | (ref_bits(w, 24, 1) << 6)),
            ref_ex6(ref_bits(w, 7, 3) | (ref_bits(w, 11, 2) << 3) | (ref_bits(w, 16, 1) << 5)) };
         const RefRGB H = { ref_ex6(ref_bits(w, 0, 1) | (ref_bits(w, 2, 5) << 1)),
            ref_ex7(ref_bits(w, 57, 7)), ref_ex6(ref_bits(w, 51, 6)) };
         const RefRGB V = { ref_ex6(ref_bits(w, 45, 6)), ref_ex7(ref_bits(w, 38, 7)), ref_ex6(ref_bits(w, 32, 6)) };
         for (uint y = 0; y < 4; y++)
            for (uint x = 0; x < 4; x++)
            {
               RefRGB p;
               p.r = ref_cl(ref_floor4(x * (H.r - O.r) + y * (V.r - O.r) + 4 * O.r + 2));
               p.g = ref_cl(ref_floor4(x * (H.g - O.g) + y * (V.g - O.g) + 4 * O.g + 2));
               p.b = ref_cl(ref_floor4(x * (H.b - O.b) + y * (V.b - O.b) + 4 * O.b + 2));
               out[y * 4 + x] = p;
            }
         return;
      }

      // Legacy individual / differential ETC1 blocks.
      const bool flip = ref_bits(w, 0, 1) != 0;
      int base[2][3];
      uint cw[2];
      if (mode == REF_IND)
      {
         base[0][0] = ref_ex4(ref_bits(w, 28, 4)); base[1][0] = ref_ex4(ref_bits(w, 24, 4));
         base[0][1] = ref_ex4(ref_bits(w, 20, 4)); base[1][1] = ref_ex4(ref_bits(w, 16, 4));
         base[0][2] = ref_ex4(ref_bits(w, 12, 4)); base[1][2] = ref_ex4(ref_bits(w, 8, 4));
      }
      else
      {
         base[0][0] = ref_ex5(ref_bits(w, 27, 5));
         base[0][1] = ref_ex5(ref_bits(w, 19, 5));
         base[0][2] = ref_ex5(ref_bits(w, 11, 5));
         base[1][0] = ref_ex5(ref_cl((int)ref_bits(w, 27, 5) + ref_sx3(ref_bits(w, 24, 3))));
         base[1][1] = ref_ex5(ref_cl((int)ref_bits(w, 19, 5) + ref_sx3(ref_bits(w, 16, 3))));
         base[1][2] = ref_ex5(ref_cl((int)ref_bits(w, 11, 5) + ref_sx3(ref_bits(w, 8, 3))));
      }
      cw[0] = ref_bits(w, 5, 3);
      cw[1] = ref_bits(w, 2, 3);
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint grp = flip ? (y < 2 ? 0 : 1) : (x < 2 ? 0 : 1);
            const uint s = kEtc1Perm[ref_selector(w, x, y)];
            const int m = kEtc1Tabs[cw[grp]][s];
            RefRGB& p = out[y * 4 + x];
            p.r = ref_cl(base[grp][0] + m);
            p.g = ref_cl(base[grp][1] + m);
            p.b = ref_cl(base[grp][2] + m);
         }
   }

   // ---------------------------------------------------------------
   // Raw fixed-block builders (each legal field is placed explicitly;
   // the detect bits are derived from the chosen differential values).
   // ---------------------------------------------------------------

   uint64 build_t(uint rA, uint gA, uint bA, uint rB, uint gB, uint bB,
      uint cw, const uint sel[16])
   {
      uint64 w = 0;
      w = ref_put(w, 27, 2, (rA >> 2) & 3);
      w = ref_put(w, 24, 2, rA & 3);
      w = ref_put(w, 20, 4, gA);
      w = ref_put(w, 16, 4, bA);
      w = ref_put(w, 12, 4, rB);
      w = ref_put(w, 8, 4, gB);
      w = ref_put(w, 4, 4, bB);
      w = ref_put(w, 2, 2, (cw >> 1) & 3);
      w = ref_put(w, 0, 1, cw & 1);
      w = ref_put(w, 1, 1, 1);
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint k = x * 4 + y, s = sel[y * 4 + x];
            w = ref_put(w, 32 + k, 1, s & 1);
            w = ref_put(w, 48 + k, 1, (s >> 1) & 1);
         }
      const uint rh = (rA >> 2) & 3, rl = rA & 3;
      if (rh + rl >= 4) { w = ref_put(w, 31, 1, 1); w = ref_put(w, 29, 2, 7); }
      else w = ref_put(w, 26, 1, 1);
      return w;
   }

   uint64 build_h(uint r0, uint g0, uint b0, uint r1, uint g1, uint b1,
      uint cw, const uint sel_in[16])
   {
      const uint p0 = (r0 << 8) | (g0 << 4) | b0;
      const uint p1 = (r1 << 8) | (g1 << 4) | b1;
      const bool swap = ((p0 < p1) ^ !(cw & 1));
      uint s0[3] = { r0, g0, b0 }, s1[3] = { r1, g1, b1 };
      uint sel[16];
      for (uint i = 0; i < 16; i++) sel[i] = sel_in[i] ^ (swap ? 2U : 0U);
      if (swap)
         for (uint c = 0; c < 3; c++) { uint t = s0[c]; s0[c] = s1[c]; s1[c] = t; }

      uint64 w = 0;
      w = ref_put(w, 27, 4, s0[0]);
      w = ref_put(w, 24, 3, (s0[1] >> 1) & 7);
      w = ref_put(w, 20, 1, s0[1] & 1);
      w = ref_put(w, 19, 1, (s0[2] >> 3) & 1);
      w = ref_put(w, 15, 3, s0[2] & 7);
      w = ref_put(w, 11, 4, s1[0]);
      w = ref_put(w, 7, 4, s1[1]);
      w = ref_put(w, 3, 4, s1[2]);
      w = ref_put(w, 2, 1, (cw >> 2) & 1);
      w = ref_put(w, 0, 1, (cw >> 1) & 1);
      w = ref_put(w, 1, 1, 1);
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint k = x * 4 + y, s = sel[y * 4 + x];
            w = ref_put(w, 32 + k, 1, s & 1);
            w = ref_put(w, 48 + k, 1, (s >> 1) & 1);
         }
      // red stays valid, green overflows.
      {
         const int rr = (int)ref_bits(w, 27, 5) + ref_sx3(ref_bits(w, 24, 3));
         if (rr < 0 || rr > 31) w = ref_put(w, 31, 1, 1);
      }
      {
         const int gg = (int)ref_bits(w, 19, 5) + ref_sx3(ref_bits(w, 16, 3));
         if (gg >= 4) { w = ref_put(w, 23, 1, 1); w = ref_put(w, 21, 3, 7); }
         else w = ref_put(w, 18, 1, 1);
      }
      return w;
   }

   uint64 build_planar(uint oR, uint oG, uint oB, uint hR, uint hG, uint hB,
      uint vR, uint vG, uint vB)
   {
      uint64 w = 0;
      w = ref_put(w, 25, 6, oR);
      w = ref_put(w, 24, 1, (oG >> 6) & 1);
      w = ref_put(w, 17, 6, oG & 63);
      w = ref_put(w, 16, 1, (oB >> 5) & 1);
      w = ref_put(w, 11, 2, (oB >> 3) & 3);
      w = ref_put(w, 7, 3, oB & 7);
      w = ref_put(w, 2, 5, (hR >> 1) & 31);
      w = ref_put(w, 0, 1, hR & 1);
      w = ref_put(w, 57, 7, hG);
      w = ref_put(w, 51, 6, hB);
      w = ref_put(w, 45, 6, vR);
      w = ref_put(w, 38, 7, vG);
      w = ref_put(w, 32, 6, vB);
      w = ref_put(w, 1, 1, 1);
      {
         const int rr = (int)ref_bits(w, 27, 5) + ref_sx3(ref_bits(w, 24, 3));
         if (rr < 0 || rr > 31) w = ref_put(w, 31, 1, 1);
      }
      {
         const int gg = (int)ref_bits(w, 19, 5) + ref_sx3(ref_bits(w, 16, 3));
         if (gg < 0 || gg > 31) w = ref_put(w, 23, 1, 1);
      }
      {
         const int bb = (int)ref_bits(w, 11, 5) + ref_sx3(ref_bits(w, 8, 3));
         if (bb >= 4) { w = ref_put(w, 13, 3, 7); }
         else w = ref_put(w, 10, 1, 1);
      }
      return w;
   }

   uint64 build_diff(uint r0, uint g0, uint b0, int dr, int dg, int db,
      uint flip, uint cw0, uint cw1, const uint sel[16])
   {
      uint64 w = 0;
      w = ref_put(w, 27, 5, r0);
      w = ref_put(w, 19, 5, g0);
      w = ref_put(w, 11, 5, b0);
      w = ref_put(w, 24, 3, dr < 0 ? dr + 8 : dr);
      w = ref_put(w, 16, 3, dg < 0 ? dg + 8 : dg);
      w = ref_put(w, 8, 3, db < 0 ? db + 8 : db);
      w = ref_put(w, 5, 3, cw0);
      w = ref_put(w, 2, 3, cw1);
      w = ref_put(w, 1, 1, 1);
      w = ref_put(w, 0, 1, flip);
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint k = x * 4 + y, s = sel[y * 4 + x];
            w = ref_put(w, 32 + k, 1, s & 1);
            w = ref_put(w, 48 + k, 1, (s >> 1) & 1);
         }
      return w;
   }

   uint64 build_ind(uint rA, uint gA, uint bA, uint rB, uint gB, uint bB,
      uint flip, uint cw0, uint cw1, const uint sel[16])
   {
      uint64 w = 0;
      w = ref_put(w, 28, 4, rA);
      w = ref_put(w, 24, 4, rB);
      w = ref_put(w, 20, 4, gA);
      w = ref_put(w, 16, 4, gB);
      w = ref_put(w, 12, 4, bA);
      w = ref_put(w, 8, 4, bB);
      w = ref_put(w, 5, 3, cw0);
      w = ref_put(w, 2, 3, cw1);
      w = ref_put(w, 1, 1, 0);
      w = ref_put(w, 0, 1, flip);
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint k = x * 4 + y, s = sel[y * 4 + x];
            w = ref_put(w, 32 + k, 1, s & 1);
            w = ref_put(w, 48 + k, 1, (s >> 1) & 1);
         }
      return w;
   }

   void store_bytes(uint64 w, uint8 out[8])
   {
      for (uint i = 0; i < 8; i++)
      {
         const uint bit = (i < 4) ? (24 - 8 * i) : (88 - 8 * i);
         out[i] = (uint8)((w >> bit) & 0xFF);
      }
   }

   const char* mode_name(etc2_mode m)
   {
      static const char* names[5] = { "individual", "differential", "t", "h", "planar" };
      return names[m];
   }

   unsigned sse_rgb(const unsigned src[16], const unsigned dec[16], uint mask = 0xFFFF)
   {
      const uint8* a = reinterpret_cast<const uint8*>(src);
      const uint8* b = reinterpret_cast<const uint8*>(dec);
      unsigned e = 0;
      for (uint i = 0; i < 16; i++)
         if (mask & (1U << i))
            for (uint c = 0; c < 3; c++)
            {
               const int d = (int)a[i * 4 + c] - (int)b[i * 4 + c];
               e += d * d;
            }
      return e;
   }

   struct LCG { uint32_t s; uint next() { s = s * 1103515245u + 12345u; return (s >> 16) & 0x7fff; } };

   void make_pixel(unsigned* px, uint idx, int r, int g, int b, int a = 255)
   {
      uint8* p = reinterpret_cast<uint8*>(&px[idx]);
      p[0] = (uint8)std::max(0, std::min(255, r));
      p[1] = (uint8)std::max(0, std::min(255, g));
      p[2] = (uint8)std::max(0, std::min(255, b));
      p[3] = (uint8)a;
   }

   std::string out_dir()
   {
      const char* d = std::getenv("TMPDIR");
      return d ? std::string(d) : std::string(".");
   }

   void write_ppm(const std::string& path, const std::vector<uint8>& rgb, uint w, uint h)
   {
      std::ofstream f(path, std::ios::binary);
      if (!f) throw std::runtime_error("cannot write PPM: " + path);
      f << "P6\n" << w << " " << h << "\n255\n";
      f.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
   }

   // ---------------------------------------------------------------
   // decode group
   // ---------------------------------------------------------------

   void test_decode_fixed(Case& c)
   {
      uint sel[16];
      for (uint i = 0; i < 16; i++) sel[i] = (i * 7 + 3) & 3;

      struct Fixed { uint64 w; RefMode want; const char* tag; };
      Fixed fixed[5];
      fixed[0] = { build_t(13, 10, 11, 8, 9, 7, 5, sel), REF_T, "T fixed" };
      fixed[1] = { build_h(14, 15, 11, 13, 11, 12, 6, sel), REF_H, "H fixed" };
      fixed[2] = { build_planar(63, 106, 50, 63, 127, 53, 57, 125, 63), REF_PLANAR, "planar fixed" };
      fixed[3] = { build_ind(1, 2, 3, 14, 13, 12, 0, 3, 7, sel), REF_IND, "individual fixed" };
      fixed[4] = { build_diff(20, 21, 22, 3, -2, 1, 1, 2, 5, sel), REF_DIFF, "differential fixed" };

      for (uint t = 0; t < 5; t++)
      {
         RefRGB ref[16];
         ref_decode(fixed[t].w, ref);

         uint8 bytes[8];
         store_bytes(fixed[t].w, bytes);
         unsigned dec[16];
         etc2_mode mode;
         const bool ok = unpack_etc2_block(bytes, dec, false, &mode);
         c.check(ok, "unpack returned false on legal block");
         c.check((RefMode)mode == fixed[t].want, "production mode disagrees with reference");
         for (uint i = 0; i < 16; i++)
         {
            const uint8* p = reinterpret_cast<const uint8*>(&dec[i]);
            std::ostringstream msg;
            msg << fixed[t].tag << " pixel " << i << " mismatch";
            c.check(p[0] == ref[i].r && p[1] == ref[i].g && p[2] == ref[i].b && p[3] == 255,
               msg.str().c_str());
         }
      }
   }

   void test_decode_boundaries(Case& c)
   {
      // Red overflow priority, both positive and negative, then H, then planar.
      struct B { uint64 w; RefMode want; };
      B cases[] =
      {
         { build_diff(31, 20, 20, 3, 0, 0, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_T },
         { build_diff(0, 20, 20, -4, 0, 0, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_T },
         { build_diff(28, 20, 20, 3, 0, 0, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_DIFF },
         { build_diff(4, 0, 20, -4, -4, 0, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_H },
         { build_diff(4, 0, 20, 0, -4, 0, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_H },
         { build_diff(4, 28, 31, 0, 0, 3, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_PLANAR },
         { build_diff(4, 4, 0, 0, 0, -4, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_PLANAR },
         { build_diff(0, 0, 0, -4, -4, -4, 0, 0, 0, std::vector<uint>(16, 0).data()), REF_T },
      };
      for (uint i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      {
         uint8 bytes[8];
         store_bytes(cases[i].w, bytes);
         etc2_mode mode;
         unsigned dec[16];
         unpack_etc2_block(bytes, dec, false, &mode);
         c.check((RefMode)mode == cases[i].want, "overflow priority mismatch");
      }
   }

   void test_decode_h_swap(Case& c)
   {
      // Same endpoint pair in both storage orders; the LSB of the distance
      // index comes from the endpoint comparison, so both must decode to the
      // same paint table after the selector flip.
      uint sel[16];
      for (uint i = 0; i < 16; i++) sel[i] = i & 3;
      const uint64 wA = build_h(10, 12, 9, 4, 5, 6, 4, sel);   // c0 > c1
      const uint64 wB = build_h(4, 5, 6, 10, 12, 9, 5, sel);    // c0 < c1, odd cw
      c.check(ref_detect(wA) == REF_H && ref_detect(wB) == REF_H, "H swap blocks misdetected");

      RefRGB ra[16], rb[16];
      ref_decode(wA, ra);
      ref_decode(wB, rb);

      uint8 bA[8], bB[8];
      store_bytes(wA, bA);
      store_bytes(wB, bB);
      unsigned da[16], db[16];
      etc2_mode ma, mb;
      unpack_etc2_block(bA, da, false, &ma);
      unpack_etc2_block(bB, db, false, &mb);
      c.check(ma == cETC2H && mb == cETC2H, "production lost H mode across swap");
      for (uint i = 0; i < 16; i++)
      {
         const uint8* pa = reinterpret_cast<const uint8*>(&da[i]);
         const uint8* pb = reinterpret_cast<const uint8*>(&db[i]);
         c.check(pa[0] == ra[i].r && pa[1] == ra[i].g && pa[2] == ra[i].b, "H order A decode");
         c.check(pb[0] == rb[i].r && pb[1] == rb[i].g && pb[2] == rb[i].b, "H order B decode");
      }
   }

   void test_decode_alpha(Case& c)
   {
      uint sel[16];
      for (uint i = 0; i < 16; i++) sel[i] = i & 3;
      const uint64 w = build_t(13, 10, 11, 8, 9, 7, 5, sel);
      uint8 bytes[8];
      store_bytes(w, bytes);

      unsigned dec[16];
      unpack_etc2_block(bytes, dec, false);
      bool all_opaque = true;
      for (uint i = 0; i < 16; i++)
         if (reinterpret_cast<uint8*>(&dec[i])[3] != 255) all_opaque = false;
      c.check(all_opaque, "alpha not defaulted to 255");

      for (uint i = 0; i < 16; i++)
         reinterpret_cast<uint8*>(&dec[i])[3] = (uint8)(i * 13 + 1);
      unpack_etc2_block(bytes, dec, true);
      bool kept = true;
      for (uint i = 0; i < 16; i++)
         if (reinterpret_cast<uint8*>(&dec[i])[3] != (uint8)(i * 13 + 1)) kept = false;
      c.check(kept, "preserve_alpha did not keep alpha bytes");
   }

   // ---------------------------------------------------------------
   // search group
   // ---------------------------------------------------------------

   void texture_t(unsigned tex[16])
   {
      // A dominant neutral cluster plus two sparse outlier clusters. T mode
      // keeps one endpoint exactly (no distance table on it) and only spends
      // the 444 endpoint on the modulated cluster, which beats H here.
      static const int pattern[16][3] =
      {
         { 128,128,128 },{ 30,220,200 },{ 128,128,128 },{ 128,128,128 },
         { 128,128,128 },{ 128,128,128 },{ 230,40,50 },{ 230,40,50 },
         { 30,220,200 },{ 30,220,200 },{ 128,128,128 },{ 230,40,50 },
         { 128,128,128 },{ 230,40,50 },{ 30,220,200 },{ 30,220,200 }
      };
      for (uint i = 0; i < 16; i++) make_pixel(tex, i, pattern[i][0], pattern[i][1], pattern[i][2]);
   }

   void texture_h(unsigned tex[16])
   {
      // Two balanced color clusters with internal spread around each center:
      // H mode modulates both endpoints.
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const uint cluster = (x / 2 + y / 2) & 1;
            const int wob = (int)((x + y) & 1) * 14 - 7;
            if (!cluster) make_pixel(tex, y * 4 + x, 220 + wob, 40 + wob / 2, 60);
            else make_pixel(tex, y * 4 + x, 30, 210 + wob / 2, 200 + wob);
         }
   }

   void texture_planar(unsigned tex[16])
   {
      // Smooth bilinear gradient with a gentle curvature: planar's three
      // anchors follow the plane almost exactly.
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
            make_pixel(tex, y * 4 + x, 10 + 15 * x + 8 * y, 200 - 20 * x - 12 * y, 30 + 40 * y);
   }

   void print_texture(const char* tag, const unsigned tex[16])
   {
      std::ostringstream os;
      os << "[" << tag << "] reproducible RGBA input (row major):";
      std::printf("%s\n", os.str().c_str());
      for (uint y = 0; y < 4; y++)
      {
         std::printf("  ");
         for (uint x = 0; x < 4; x++)
         {
            const uint8* p = reinterpret_cast<const uint8*>(&tex[y * 4 + x]);
            std::printf("%02x%02x%02x%02x ", p[0], p[1], p[2], p[3]);
         }
         std::printf("\n");
      }
   }

   void run_mode_case(Case& c, etc2_mode want, const char* tag, void (*make)(unsigned[16]))
   {
      etc2_options full;
      etc2_options restricted = full;
      if (want == cETC2T) restricted.m_allow_t = false;
      if (want == cETC2H) restricted.m_allow_h = false;
      if (want == cETC2Planar) restricted.m_allow_planar = false;

      unsigned tex[16];
      make(tex);
      print_texture(tag, tex);

      uint8 bFull[8], bRest[8];
      const unsigned eFull = pack_etc2_block(bFull, tex, full);
      const unsigned eRest = pack_etc2_block(bRest, tex, restricted);

      unsigned dec[16];
      etc2_mode mode;
      unpack_etc2_block(bFull, dec, false, &mode);
      const unsigned true_error = sse_rgb(tex, dec);
      std::printf("[%s] selected=%s reported_error=%u true_decoded_error=%u restricted_error=%u\n",
         tag, mode_name(mode), eFull, true_error, eRest);

      c.check(mode == want, "expected mode was not selected");
      c.check(eFull == true_error, "reported error is not the post-reconstruction SSE");
      c.check(eFull < eRest, "enabling the mode did not strictly improve distortion");
      c.check(true_error < eRest, "quantized improvement not visible in decoded pixels");
   }

   void test_search_modes(Case& c)
   {
      run_mode_case(c, cETC2T, "search-t", texture_t);
      run_mode_case(c, cETC2H, "search-h", texture_h);
      run_mode_case(c, cETC2Planar, "search-planar", texture_planar);
   }

   void test_search_quantized(Case& c)
   {
      // Counterexample: a continuous least-squares plane fit has one error,
      // but the emitted planar block quantizes three RGB676 anchors and its
      // actual integer reconstruction has a different (larger) error. The
      // encoder must report the quantized number, never the continuous fit.
      unsigned tex[16];
      texture_planar(tex);

      double sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0;
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const double z = reinterpret_cast<uint8*>(&tex[y * 4 + x])[1]; // green channel
            sx += x; sy += y; sz += z; sxx += x * x; syy += y * y; sxy += x * y;
            sxz += x * z; syz += y * z;
         }
      const double n = 16.0;
      const double A = n * sxx - sx * sx;
      const double B = n * sxy - sx * sy;
      const double C = n * syy - sy * sy;
      const double det = A * C - B * B;
      const double rx = n * sxz - sx * sz;
      const double ry = n * syz - sy * sz;
      const double px = (rx * C - ry * B) / det;
      const double py = (A * ry - B * rx) / det;
      const double c0 = (sz - px * sx - py * sy) / n;
      unsigned continuous = 0;
      for (uint y = 0; y < 4; y++)
         for (uint x = 0; x < 4; x++)
         {
            const double fit = px * x + py * y + c0;
            const int have = reinterpret_cast<uint8*>(&tex[y * 4 + x])[1];
            const int d = (int)lround(fit) - have;
            continuous += d * d;
         }

      etc2_options opt;
      uint8 blk[8];
      const unsigned reported = pack_etc2_block(blk, tex, opt);
      unsigned dec[16];
      etc2_mode mode;
      unpack_etc2_block(blk, dec, false, &mode);
      const unsigned quantized = sse_rgb(tex, dec);
      std::printf("[search-quant] mode=%s continuous_green_sse=%u quantized_block_sse=%u reported=%u\n",
         mode_name(mode), continuous, quantized, reported);
      c.check(continuous != quantized, "continuous and quantized errors unexpectedly equal");
      c.check(reported == quantized, "encoder must score the quantized reconstruction");
   }

   void test_search_monotonic(Case& c)
   {
      // Enabling more modes can never worsen the result.
      LCG lcg = { 0xC0FFEE };
      etc2_options full;
      etc2_options noT = full, noH = full, noP = full, none = full;
      noT.m_allow_t = false; noH.m_allow_h = false; noP.m_allow_planar = false;
      none.m_allow_t = none.m_allow_h = none.m_allow_planar = false;

      for (uint trial = 0; trial < 120; trial++)
      {
         unsigned tex[16];
         for (uint i = 0; i < 16; i++)
            make_pixel(tex, i, lcg.next() % 256, lcg.next() % 256, lcg.next() % 256);
         uint8 b[5][8];
         const unsigned eFull = pack_etc2_block(b[0], tex, full);
         const unsigned eNoT = pack_etc2_block(b[1], tex, noT);
         const unsigned eNoH = pack_etc2_block(b[2], tex, noH);
         const unsigned eNoP = pack_etc2_block(b[3], tex, noP);
         const unsigned eNone = pack_etc2_block(b[4], tex, none);
         c.check(eFull <= eNoT && eFull <= eNoH && eFull <= eNoP && eFull <= eNone,
            "adding a mode made the score worse");
      }
   }

   // ---------------------------------------------------------------
   // images group
   // ---------------------------------------------------------------

   std::vector<uint8> decode_image(const std::vector<uint8>& packed, uint blocksX, uint blocksY)
   {
      std::vector<uint8> rgb(blocksX * 4 * blocksY * 4 * 3);
      for (uint by = 0; by < blocksY; by++)
         for (uint bx = 0; bx < blocksX; bx++)
         {
            unsigned dec[16];
            unpack_etc2_block(&packed[(by * blocksX + bx) * 8], dec, false);
            for (uint y = 0; y < 4; y++)
               for (uint x = 0; x < 4; x++)
               {
                  const uint8* p = reinterpret_cast<const uint8*>(&dec[y * 4 + x]);
                  uint8* q = &rgb[((by * 4 + y) * blocksX * 4 + (bx * 4 + x)) * 3];
                  q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
               }
         }
      return rgb;
   }

   void check_image(Case& c, uint width, uint height, size_t stride, uint8 sentinel, const char* ppm_name)
   {
      std::vector<uint8> src(stride * height, sentinel);
      for (uint y = 0; y < height; y++)
         for (uint x = 0; x < width; x++)
         {
            uint8* p = &src[y * stride + x * 4];
            p[0] = (uint8)(20 + 13 * x + 7 * y);
            p[1] = (uint8)(200 - 9 * x - 11 * y);
            p[2] = (uint8)(30 + 17 * y + 5 * x);
            p[3] = 255;
         }

      etc2_options opt;
      const std::vector<uint8> packed = pack_etc2_image(src.data(), width, height, stride, opt);
      const uint blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
      c.check(packed.size() == (size_t)blocksX * blocksY * 8, "packed image size");

      // Block order is rows of blocks first; verify every real pixel round-trips.
      const std::vector<uint8> rgb = decode_image(packed, blocksX, blocksY);
      unsigned real_error = 0;
      for (uint y = 0; y < height; y++)
         for (uint x = 0; x < width; x++)
         {
            const uint8* s = &src[y * stride + x * 4];
            const uint8* d = &rgb[(y * blocksX * 4 + x) * 3];
            for (uint k = 0; k < 3; k++)
            {
               const int diff = (int)s[k] - (int)d[k];
               real_error += diff * diff;
            }
         }
      std::printf("[images %ux%u stride=%zu] blocks=%ux%u real_pixel_sse=%u\n",
         width, height, stride, blocksX, blocksY, real_error);

      std::vector<uint8> cropped(width * height * 3);
      for (uint y = 0; y < height; y++)
         memcpy(&cropped[(y * width) * 3], &rgb[(y * blocksX * 4) * 3], width * 3);
      write_ppm(out_dir() + "/" + ppm_name, cropped, width, height);

      // Padding sentinels must never be read: change every padding byte and
      // require an identical block stream.
      std::vector<uint8> src2 = src;
      for (uint y = 0; y < height; y++)
         for (size_t k = width * 4; k < stride; k++)
            src2[y * stride + k] = (uint8)~sentinel;
      const std::vector<uint8> packed2 = pack_etc2_image(src2.data(), width, height, stride, opt);
      c.check(packed == packed2, "row padding bytes influenced the block stream");
   }

   void test_images_sizes(Case& c)
   {
      check_image(c, 1, 1, 4, 0xA5, "etc2_test_1x1.ppm");
      check_image(c, 5, 7, 5 * 4 + 9, 0x5A, "etc2_test_5x7.ppm");
      check_image(c, 4, 4, 4 * 4 + 4, 0x33, "etc2_test_4x4.ppm");
   }

   void test_edge_mask(Case& c)
   {
      // Pixels outside valid_mask must not affect the stream: same valid
      // pixels but different invalid pixels produce identical bytes.
      unsigned a[16], b[16];
      LCG lcg = { 777 };
      for (uint i = 0; i < 16; i++)
         make_pixel(a, i, lcg.next() % 256, lcg.next() % 256, lcg.next() % 256);
      memcpy(b, a, sizeof(a));
      const uint mask = 0x0777U; // first three rows valid, last row replicated
      for (uint i = 0; i < 16; i++)
         if (!(mask & (1U << i)))
            make_pixel(b, i, 255 - reinterpret_cast<uint8*>(&a[i])[0], 17, 99);

      etc2_options opt;
      uint8 ba[8], bb[8];
      const unsigned ea = pack_etc2_block(ba, a, opt, mask);
      const unsigned eb = pack_etc2_block(bb, b, opt, mask);
      c.check(memcmp(ba, bb, 8) == 0, "invalid pixels leaked into the masked search");
      c.check(ea == eb, "masked error differs across identical valid pixels");

      // The same exclusion drives the partial bottom block of a 4x5 image.
      std::vector<uint8> img(4 * 5 * 4);
      for (uint y = 0; y < 5; y++)
         for (uint x = 0; x < 4; x++)
         {
            uint8* p = &img[(y * 4 + x) * 4];
            p[0] = (uint8)(40 + 20 * x); p[1] = (uint8)(60 + 15 * y); p[2] = 90; p[3] = 255;
         }
      const std::vector<uint8> packed = pack_etc2_image(img.data(), 4, 5, 16, opt);
      c.check(packed.size() == 16, "4x5 image must yield two 8-byte blocks");
      unsigned dec[16];
      etc2_mode mode;
      unpack_etc2_block(&packed[8], dec, false, &mode);
      std::printf("[images edge-mask] bottom block mode=%s masked_error=%u\n", mode_name(mode), eb);
   }

   void test_image_args(Case& c)
   {
      etc2_options opt;
      uint8 px[64] = { 0 };
      bool threw = false;
      try { pack_etc2_image(px, 0, 4, 16, opt); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "zero width accepted");
      threw = false;
      try { pack_etc2_image(px, 4, 0, 16, opt); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "zero height accepted");
      threw = false;
      try { pack_etc2_image(px, 4, 4, 15, opt); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "stride below width*4 accepted");
      threw = false;
      try { pack_etc2_image(nullptr, 4, 4, 16, opt); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "null source accepted");
   }

   // ---------------------------------------------------------------
   // compatibility group
   // ---------------------------------------------------------------

   void test_compat_identity(Case& c)
   {
      LCG lcg = { 0xBADF00D };
      for (uint q = 0; q < 3; q++)
      {
         etc2_options off;
         off.m_allow_t = off.m_allow_h = off.m_allow_planar = false;
         off.m_quality = (etc1_quality)q;
         etc1_pack_params pp;
         pp.m_quality = (etc1_quality)q;
         pp.m_dithering = false;

         for (uint trial = 0; trial < 64; trial++)
         {
            unsigned tex[16];
            for (uint i = 0; i < 16; i++)
               make_pixel(tex, i, lcg.next() % 256, lcg.next() % 256, lcg.next() % 256, 255);
            uint8 b1[8], b2[8];
            pack_etc1_block(b1, tex, pp);
            pack_etc2_block(b2, tex, off);
            c.check(memcmp(b1, b2, 8) == 0, "all-modes-off output differs from pack_etc1_block");
         }

         // Explicit solid blocks exercise the original solid-color fast path.
         for (uint s = 0; s < 16; s++)
         {
            unsigned tex[16];
            for (uint i = 0; i < 16; i++)
               make_pixel(tex, i, (s * 17) & 255, (s * 31 + 40) & 255, (s * 53 + 7) & 255, 255);
            uint8 b1[8], b2[8];
            pack_etc1_block(b1, tex, pp);
            pack_etc2_block(b2, tex, off);
            c.check(memcmp(b1, b2, 8) == 0, "solid block identity mismatch");
         }
      }
   }

   void test_compat_qualities_stability(Case& c)
   {
      LCG lcg = { 555 };
      etc2_options opt;
      for (uint q = 0; q < 3; q++)
      {
         opt.m_quality = (etc1_quality)q;
         for (uint trial = 0; trial < 24; trial++)
         {
            unsigned tex[16];
            for (uint i = 0; i < 16; i++)
               make_pixel(tex, i, lcg.next() % 256, lcg.next() % 256, lcg.next() % 256);
            unsigned backup[16];
            memcpy(backup, tex, sizeof(tex));
            uint8 b1[8], b2[8];
            const unsigned e1 = pack_etc2_block(b1, tex, opt);
            const unsigned e2 = pack_etc2_block(b2, tex, opt);
            c.check(memcmp(backup, tex, sizeof(tex)) == 0, "encoder modified its input");
            c.check(memcmp(b1, b2, 8) == 0, "same input produced different streams");
            c.check(e1 == e2, "same input produced different reported errors");
            unsigned dec[16];
            unpack_etc2_block(b1, dec, false);
            c.check(sse_rgb(tex, dec) == e1, "quality run error mismatch");
         }
      }
   }

   void test_compat_threads(Case& c)
   {
      const uint N = 256;
      std::vector<unsigned> tex(N * 16);
      LCG lcg = { 31337 };
      for (uint i = 0; i < N * 16; i++)
         make_pixel(tex.data(), i, lcg.next() % 256, lcg.next() % 256, lcg.next() % 256);

      etc2_options opt;
      std::vector<uint8> serial(N * 8);
      for (uint i = 0; i < N; i++)
         pack_etc2_block(&serial[i * 8], &tex[i * 16], opt);

      std::vector<uint8> parallel(N * 8);
      std::atomic<uint> errors{ 0 };
      auto worker = [&](uint tid)
      {
         try
         {
            for (uint i = tid; i < N; i += 4)
               pack_etc2_block(&parallel[i * 8], &tex[i * 16], opt);
         }
         catch (...) { errors++; }
      };
      std::thread t[4];
      for (uint i = 0; i < 4; i++) t[i] = std::thread(worker, i);
      for (uint i = 0; i < 4; i++) t[i].join();
      c.check(errors.load() == 0, "threaded encoding threw");
      c.check(memcmp(serial.data(), parallel.data(), N * 8) == 0,
         "multithreaded output diverged from serial output");

      // Interleave old and new interfaces on the same threads.
      std::atomic<bool> ok{ true };
      etc1_pack_params pp;
      pp.m_dithering = false;
      auto mixed = [&](uint tid)
      {
         for (uint i = tid; i < N; i += 4)
         {
            uint8 a[8], b[8];
            pack_etc1_block(a, &tex[i * 16], pp);
            pack_etc2_block(b, &tex[i * 16], opt);
            etc2_mode m;
            unsigned dec[16];
            if (!unpack_etc2_block(b, dec, false, &m)) ok = false;
         }
      };
      for (uint i = 0; i < 4; i++) t[i] = std::thread(mixed, i);
      for (uint i = 0; i < 4; i++) t[i].join();
      c.check(ok.load(), "interleaved ETC1/ETC2 encoding failed");
   }

   void test_compat_block_args(Case& c)
   {
      unsigned tex[16];
      for (uint i = 0; i < 16; i++) make_pixel(tex, i, 10, 20, 30);
      etc2_options opt;
      uint8 blk[8];
      bool threw = false;
      try { pack_etc2_block(blk, tex, opt, 0); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "zero valid_mask accepted");
      threw = false;
      etc2_options bad = opt;
      bad.m_quality = (etc1_quality)99;
      try { pack_etc2_block(blk, tex, bad); } catch (const std::invalid_argument&) { threw = true; }
      c.check(threw, "invalid quality accepted");
   }
}

void register_task_tests(Registry& r)
{
   r.add("decode", "fixed-blocks-independent-reference", test_decode_fixed);
   r.add("decode", "mode-trigger-boundaries-and-priority", test_decode_boundaries);
   r.add("decode", "h-endpoint-swap-distance-index", test_decode_h_swap);
   r.add("decode", "alpha-preservation", test_decode_alpha);

   r.add("search", "each-new-mode-selected-and-strictly-better", test_search_modes);
   r.add("search", "continuous-fit-vs-quantized-error", test_search_quantized);
   r.add("search", "enabling-modes-never-worsens", test_search_monotonic);

   r.add("images", "sizes-1x1-5x7-and-sentinels", test_images_sizes);
   r.add("images", "edge-valid-mask-exclusion", test_edge_mask);
   r.add("images", "invalid-arguments", test_image_args);

   r.add("compatibility", "etc1-byte-identity-all-modes-off", test_compat_identity);
   r.add("compatibility", "three-qualities-and-stability", test_compat_qualities_stability);
   r.add("compatibility", "multithreaded-interleaved-encoding", test_compat_threads);
   r.add("compatibility", "block-invalid-arguments", test_compat_block_args);
}
