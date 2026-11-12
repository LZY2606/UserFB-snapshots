/*
tests/lz77_index.cpp

Verification for the suffix-array indexed LZ77 matcher in lodepng.cpp. It
includes the production implementation directly so internal LZ77 tokens can be
inspected, and includes the unmodified reference implementation in a separate
namespace for token- and byte-exact comparison.

The LODEPNG_LZ77_TEST work counters are enabled for the production namespace
only; the reference namespace stays the untouched stock source.
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

#include "verification/driver.h"

/* Unmodified stock implementation, isolated in its own namespace. */
namespace stock {
#include "verification/reference/lodepng.cpp"
}

#undef LODEPNG_H
/* Enable the test-only work counters for the production translation unit. */
#define LODEPNG_LZ77_TEST 1
namespace target {
#include "lodepng.cpp"
}

namespace {

typedef std::vector<unsigned char> Bytes;

#define CHECK(cond) do { if(!(cond)) { \
  std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
  return 0; } } while(0)

/* ------------------------------------------------------------- input data */

/* Exact generator from verification/baseline.cpp: 640 segments, 240 bytes of
65 each and 18 varying tail bytes, total length 165120. */
static Bytes makeBaseline() {
  Bytes raw;
  for(unsigned r = 0; r < 640; ++r) {
    for(unsigned i = 0; i < 240; ++i) raw.push_back(65);
    for(unsigned j = 0; j < 18; ++j)
      raw.push_back((unsigned char)(((r * 37 + j * 53 + (r >> (j % 7))) % 251 + 1) & 0xff));
  }
  return raw;
}

static Bytes makeRandom(size_t n, unsigned seed) {
  Bytes v(n);
  unsigned x = seed ? seed : 1;
  for(size_t i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    v[i] = (unsigned char)(x >> 16);
  }
  return v;
}

static Bytes makeLowAlpha(size_t n, unsigned seed) {
  Bytes v(n);
  unsigned x = seed ? seed : 1;
  for(size_t i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; v[i] = (unsigned char)(x % 4); }
  return v;
}

static Bytes makePrefixTail(size_t segments) {
  Bytes v;
  for(unsigned r = 0; r < segments; ++r) {
    for(unsigned i = 0; i < 200; ++i) v.push_back(65);
    for(unsigned j = 0; j < 10; ++j) v.push_back((unsigned char)((r * 37 + j * 53) % 251 + 1));
  }
  return v;
}

static Bytes makePeriodic(size_t n, unsigned period) {
  Bytes v(n);
  for(size_t i = 0; i < n; ++i) v[i] = (unsigned char)((i % period) + 1);
  return v;
}

/* ------------------------------------------------------ token level checks */

struct LZSettings { unsigned btype, window, minmatch, nicematch, lazy; };

/* Run encodeLZ77 block by block with a persistent hash (and one whole-input
index), concatenating the internal token streams exactly as lodepng_deflatev
drives the encoder. */
static bool tokenStreams(const Bytes& in, const std::vector<size_t>& ends,
                         const LZSettings& st, bool useTarget,
                         std::vector<unsigned>* tokens) {
  size_t n = in.size();
  if(useTarget) {
    target::Hash hash;
    target::LZ77Index ix;
    target::uivector out;
    target::uivector_init(&out);
    if(target::hash_init(&hash, st.window)) return false;
    unsigned e = target::lz77_index_build(&ix, in.data(), 0, n,
                                          n ? ends.data() : 0, ends.size());
    if(e) { target::hash_cleanup(&hash); return false; }
    size_t start = 0;
    for(size_t b = 0; b < ends.size(); ++b) {
      e = target::encodeLZ77(&out, &hash, &ix, in.data(), start, ends[b],
                             st.window, st.minmatch, st.nicematch, st.lazy);
      if(e) { target::lz77_index_cleanup(&ix); target::hash_cleanup(&hash);
              target::uivector_cleanup(&out); return false; }
      start = ends[b];
    }
    tokens->assign(out.data, out.data + out.size);
    target::lz77_index_cleanup(&ix);
    target::hash_cleanup(&hash);
    target::uivector_cleanup(&out);
  } else {
    stock::Hash hash;
    stock::uivector out;
    stock::uivector_init(&out);
    if(stock::hash_init(&hash, st.window)) return false;
    size_t start = 0;
    for(size_t b = 0; b < ends.size(); ++b) {
      unsigned e = stock::encodeLZ77(&out, &hash, in.data(), start, ends[b],
                                     st.window, st.minmatch, st.nicematch, st.lazy);
      if(e) { stock::hash_cleanup(&hash); stock::uivector_cleanup(&out); return false; }
      start = ends[b];
    }
    tokens->assign(out.data, out.data + out.size);
    stock::hash_cleanup(&hash);
    stock::uivector_cleanup(&out);
  }
  return true;
}

static bool tokensEqual(const Bytes& in, const std::vector<size_t>& ends,
                        const LZSettings& st) {
  std::vector<unsigned> a, b;
  if(!tokenStreams(in, ends, st, false, &a)) return false;
  if(!tokenStreams(in, ends, st, true, &b)) return false;
  if(a.size() != b.size()) {
    std::fprintf(stderr, "token size %zu vs %zu\n", a.size(), b.size());
    return false;
  }
  for(size_t i = 0; i < a.size(); ++i) if(a[i] != b[i]) {
    std::fprintf(stderr, "token %zu: stock=%u target=%u\n", i, a[i], b[i]);
    return false;
  }
  return true;
}

/* -------------------------------------------------------- public zlib bytes */

static Bytes zlibStock(const Bytes& in, const LZSettings& st) {
  stock::LodePNGCompressSettings s;
  stock::lodepng_compress_settings_init(&s);
  s.btype = st.btype; s.windowsize = st.window; s.minmatch = st.minmatch;
  s.nicematch = st.nicematch; s.lazymatching = st.lazy;
  unsigned char* out = 0; size_t n = 0;
  unsigned e = stock::lodepng_zlib_compress(&out, &n, in.data(), in.size(), &s);
  Bytes r;
  if(!e) r.assign(out, out + n);
  free(out);
  return r;
}

static Bytes zlibTarget(const Bytes& in, const LZSettings& st) {
  target::LodePNGCompressSettings s;
  target::lodepng_compress_settings_init(&s);
  s.btype = st.btype; s.windowsize = st.window; s.minmatch = st.minmatch;
  s.nicematch = st.nicematch; s.lazymatching = st.lazy;
  unsigned char* out = 0; size_t n = 0;
  unsigned e = target::lodepng_zlib_compress(&out, &n, in.data(), in.size(), &s);
  Bytes r;
  if(!e) r.assign(out, out + n);
  free(out);
  return r;
}

static bool bytesEqual(const Bytes& a, const Bytes& b) {
  return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}

static const char* tmpDir() {
  const char* d = getenv("TMPDIR");
  if(d && *d) return d;
  if(P_tmpdir && *P_tmpdir) return P_tmpdir;
  return ".";
}

/* Invoke the externally built stock runner (STOCK_ENCODER) and compare its
zlib stream byte-for-byte with the production output. */
static bool stockRunnerMatches(const Bytes& in, const LZSettings& st) {
  const char* exe = getenv("STOCK_ENCODER");
  if(!exe) { std::fprintf(stderr, "STOCK_ENCODER not set\n"); return false; }
  static unsigned counter = 0;
  char ipath[512], opath[512], cmd[1536];
  snprintf(ipath, sizeof(ipath), "%s/lz77_in_%d_%u.bin",
           tmpDir(), (int)getpid(), counter);
  snprintf(opath, sizeof(opath), "%s/lz77_out_%d_%u.z",
           tmpDir(), (int)getpid(), counter);
  ++counter;
  {
    std::ofstream f(ipath, std::ios::binary);
    if(!f) return false;
    if(!in.empty()) f.write((const char*)in.data(), (std::streamsize)in.size());
  }
  snprintf(cmd, sizeof(cmd),
           "\"%s\" \"%s\" \"%s\" %u %u %u %u %u",
           exe, ipath, opath, st.btype, st.window, st.minmatch,
           st.nicematch, st.lazy);
  int rc = system(cmd);
  bool ok = (rc == 0);
  if(!ok) std::fprintf(stderr, "stock runner rc=%d cmd=%s\n", rc, cmd);
  if(ok) {
    std::ifstream f(opath, std::ios::binary);
    Bytes ref((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Bytes got = zlibTarget(in, st);
    ok = bytesEqual(ref, got);
    if(!ok) std::fprintf(stderr, "runner mismatch w=%u nice=%u lazy=%u\n",
                         st.window, st.nicematch, st.lazy);
  }
  remove(ipath);
  remove(opath);
  return ok;
}

static const unsigned WINDOWS[] = {256, 1024, 2048, 8192, 16384, 32768};
static const unsigned NICEMATCHES[] = {8, 128, 258};

/* ------------------------------------------------ special encoder modes */

static unsigned custom_deflate_stock(unsigned char** out, size_t* n,
                                     const unsigned char* in, size_t insize,
                                     const stock::LodePNGCompressSettings* s) {
  stock::LodePNGCompressSettings t = *s; t.custom_deflate = 0;
  return stock::lodepng_deflate(out, n, in, insize, &t);
}
static unsigned custom_deflate_target(unsigned char** out, size_t* n,
                                      const unsigned char* in, size_t insize,
                                      const target::LodePNGCompressSettings* s) {
  target::LodePNGCompressSettings t = *s; t.custom_deflate = 0;
  return target::lodepng_deflate(out, n, in, insize, &t);
}
static unsigned custom_zlib_stock(unsigned char** out, size_t* n,
                                  const unsigned char* in, size_t insize,
                                  const stock::LodePNGCompressSettings* s) {
  stock::LodePNGCompressSettings t = *s; t.custom_zlib = 0;
  return stock::lodepng_zlib_compress(out, n, in, insize, &t);
}
static unsigned custom_zlib_target(unsigned char** out, size_t* n,
                                   const unsigned char* in, size_t insize,
                                   const target::LodePNGCompressSettings* s) {
  target::LodePNGCompressSettings t = *s; t.custom_zlib = 0;
  return target::lodepng_zlib_compress(out, n, in, insize, &t);
}

static bool runSpecialModes() {
  Bytes in = makePrefixTail(30);
  /* default settings, untouched */
  {
    stock::LodePNGCompressSettings a; stock::lodepng_compress_settings_init(&a);
    target::LodePNGCompressSettings b; target::lodepng_compress_settings_init(&b);
    unsigned char *za=0,*zb=0; size_t na=0,nb=0;
    CHECK(!stock::lodepng_zlib_compress(&za,&na,in.data(),in.size(),&a));
    CHECK(!target::lodepng_zlib_compress(&zb,&nb,in.data(),in.size(),&b));
    CHECK(na==nb && !memcmp(za,zb,na));
    free(za); free(zb);
  }
  /* use_lz77=0, btype 1 and 2 */
  for(unsigned bt = 1; bt <= 2; ++bt) {
    stock::LodePNGCompressSettings a; stock::lodepng_compress_settings_init(&a); a.use_lz77=0; a.btype=bt;
    target::LodePNGCompressSettings b; target::lodepng_compress_settings_init(&b); b.use_lz77=0; b.btype=bt;
    unsigned char *za=0,*zb=0; size_t na=0,nb=0;
    CHECK(!stock::lodepng_zlib_compress(&za,&na,in.data(),in.size(),&a));
    CHECK(!target::lodepng_zlib_compress(&zb,&nb,in.data(),in.size(),&b));
    CHECK(na==nb && !memcmp(za,zb,na));
    free(za); free(zb);
  }
  /* btype=0 uncompressed blocks, including a >65535-byte input */
  {
    Bytes large = makeRandom(200000, 71);
    const Bytes* cases[2] = {&in, &large};
    for(int c = 0; c < 2; ++c) {
      stock::LodePNGCompressSettings a; stock::lodepng_compress_settings_init(&a); a.btype=0;
      target::LodePNGCompressSettings b; target::lodepng_compress_settings_init(&b); b.btype=0;
      unsigned char *za=0,*zb=0; size_t na=0,nb=0;
      CHECK(!stock::lodepng_zlib_compress(&za,&na,cases[c]->data(),cases[c]->size(),&a));
      CHECK(!target::lodepng_zlib_compress(&zb,&nb,cases[c]->data(),cases[c]->size(),&b));
      CHECK(na==nb && !memcmp(za,zb,na));
      free(za); free(zb);
    }
  }
  /* custom_deflate and custom_zlib callbacks: dispatch must be unchanged */
  {
    stock::LodePNGCompressSettings a; stock::lodepng_compress_settings_init(&a);
    target::LodePNGCompressSettings b; target::lodepng_compress_settings_init(&b);
    a.custom_deflate = custom_deflate_stock; b.custom_deflate = custom_deflate_target;
    unsigned char *za=0,*zb=0; size_t na=0,nb=0;
    CHECK(!stock::lodepng_zlib_compress(&za,&na,in.data(),in.size(),&a));
    CHECK(!target::lodepng_zlib_compress(&zb,&nb,in.data(),in.size(),&b));
    CHECK(na==nb && !memcmp(za,zb,na));
    free(za); free(zb);
    a.custom_deflate = 0; b.custom_deflate = 0;
    a.custom_zlib = custom_zlib_stock; b.custom_zlib = custom_zlib_target;
    CHECK(!stock::lodepng_zlib_compress(&za,&na,in.data(),in.size(),&a));
    CHECK(!target::lodepng_zlib_compress(&zb,&nb,in.data(),in.size(),&b));
    CHECK(na==nb && !memcmp(za,zb,na));
    free(za); free(zb);
  }
  return true;
}

/* =================================================================== groups */

/* 1. legacy-stream: broad settings matrix, tokens and public zlib bytes. */
static int run_legacy_stream() {
  Bytes inputs[] = {
    Bytes(),                          /* empty */
    Bytes(1, 7),                      /* one byte */
    Bytes(2, 9),                      /* two byte tail */
    makePeriodic(1000, 1),            /* repeated zero-like run (value 1) */
    Bytes(5000, 0),                   /* repeated zeros */
    Bytes(5000, 65),                  /* repeated non-zero */
    makePrefixTail(40),               /* same prefix, different tails */
    makePeriodic(8000, 7),            /* periodic */
    makeRandom(9000, 11),
    makeLowAlpha(9000, 23)
  };
  for(size_t ii = 0; ii < sizeof(inputs) / sizeof(inputs[0]); ++ii) {
    const Bytes& in = inputs[ii];
    std::vector<size_t> ends(1, in.size());
    for(unsigned bt = 1; bt <= 2; ++bt) {
      for(size_t wi = 0; wi < sizeof(WINDOWS) / sizeof(WINDOWS[0]); ++wi) {
        for(size_t ni = 0; ni < sizeof(NICEMATCHES) / sizeof(NICEMATCHES[0]); ++ni) {
          for(unsigned mm = 3; mm <= 6; mm += 3) {
            for(unsigned lz = 0; lz <= 1; ++lz) {
              LZSettings st = {bt, WINDOWS[wi], mm, NICEMATCHES[ni], lz};
              CHECK(tokensEqual(in, ends, st));
              CHECK(bytesEqual(zlibStock(in, st), zlibTarget(in, st)));
            }
          }
        }
      }
    }
  }
  /* representative independent-process cross check against the stock binary */
  LZSettings reps[] = {
    {1, 32768, 3, 258, 1}, {2, 32768, 3, 128, 0},
    {2, 8192, 6, 8, 1},    {1, 2048, 3, 128, 1},
    {2, 16384, 3, 258, 0}
  };
  for(size_t i = 0; i < sizeof(reps) / sizeof(reps[0]); ++i)
    CHECK(stockRunnerMatches(makeBaseline(), reps[i]));
  CHECK(runSpecialModes());
  return 1;
}

/* 2. lazy-window: lazy on/off across window wrap-around. */
static int run_lazy_window() {
  Bytes big = makePrefixTail(400);                 /* longer than two windows */
  CHECK(big.size() > 32768u * 2);
  Bytes reps2[] = {big, makeRandom(70000, 5), makeLowAlpha(70000, 9),
                   Bytes(70000, 0), Bytes(70000, 65)};
  for(size_t ii = 0; ii < sizeof(reps2) / sizeof(reps2[0]); ++ii) {
    const Bytes& in = reps2[ii];
    std::vector<size_t> ends(1, in.size());
    for(unsigned lz = 0; lz <= 1; ++lz) {
      LZSettings st1 = {2, 8192, 3, 258, lz};
      LZSettings st2 = {1, 32768, 3, 128, lz};
      LZSettings st3 = {2, 32768, 6, 64, lz};
      CHECK(tokensEqual(in, ends, st1));
      CHECK(tokensEqual(in, ends, st2));
      CHECK(tokensEqual(in, ends, st3));
      CHECK(bytesEqual(zlibStock(in, st1), zlibTarget(in, st1)));
      CHECK(bytesEqual(zlibStock(in, st2), zlibTarget(in, st2)));
      CHECK(bytesEqual(zlibStock(in, st3), zlibTarget(in, st3)));
    }
  }
  return 1;
}

/* 3. block-history: more than three deflate blocks and cross-block history. */
static int run_block_history() {
  /* ~900 KB forces at least four 262144-byte dynamic blocks */
  Bytes in = makePrefixTail(4400);
  while(in.size() < 900000u) {
    Bytes more = makeLowAlpha(60000, (unsigned)(in.size() & 0xffff));
    in.insert(in.end(), more.begin(), more.end());
  }
  /* explicit multi-block token boundaries in addition to real block sizes */
  std::vector<size_t> ends;
  ends.push_back(100000);
  ends.push_back(262144);
  ends.push_back(524288);
  ends.push_back(786432);
  ends.push_back(in.size());
  LZSettings st = {2, 32768, 3, 258, 1};
  CHECK(tokensEqual(in, ends, st));
  /* full public streams: dynamic blocks chosen inside lodepng, fixed is one block */
  LZSettings dyn = {2, 32768, 3, 258, 1};
  LZSettings fix = {1, 32768, 3, 258, 1};
  CHECK(bytesEqual(zlibStock(in, dyn), zlibTarget(in, dyn)));
  CHECK(bytesEqual(zlibStock(in, fix), zlibTarget(in, fix)));
  CHECK(stockRunnerMatches(in, dyn));
  /* round-trip safety on the multi-block stream */
  Bytes z = zlibTarget(in, dyn);
  unsigned char* dec = 0; size_t nd = 0;
  CHECK(!target::lodepng_zlib_decompress(&dec, &nd, z.data(), z.size(),
                                         &target::lodepng_default_decompress_settings));
  CHECK(nd == in.size() && !memcmp(dec, in.data(), nd));
  free(dec);
  return 1;
}

/* 4. index-work: the quarter-budget counter contract on the fixed input. */
static int run_index_work() {
  Bytes in = makeBaseline();
  CHECK(in.size() == 165120u);
  LZSettings st = {1, 32768, 3, 258, 1};
  std::vector<size_t> ends(1, in.size());

  target::lodepng_lz77test_byte_comparisons = 0;
  target::lodepng_lz77test_candidate_checks = 0;
  std::vector<unsigned> got, ref;
  CHECK(tokenStreams(in, ends, st, true, &got));
  unsigned long long bytes = target::lodepng_lz77test_byte_comparisons;
  unsigned long long cands = target::lodepng_lz77test_candidate_checks;
  CHECK(tokenStreams(in, ends, st, false, &ref));
  CHECK(got == ref);
  CHECK(got.size() == 14163u);

  /* stock figures documented for this exact input; each must drop by >= 4x */
  const unsigned long long stock_bytes = 4254727288ull;
  const unsigned long long stock_cands = 34742697ull;
  CHECK(bytes * 4ull <= stock_bytes);
  CHECK(cands * 4ull <= stock_cands);
  std::printf("index-work: byte_comparisons=%llu (stock %llu, 1/%.1f), "
              "candidate_checks=%llu (stock %llu, 1/%.1f), token_elements=%zu\n",
              bytes, stock_bytes, (double)stock_bytes / (double)bytes,
              cands, stock_cands, (double)stock_cands / (double)cands,
              got.size());

  /* index build must also be counted: a purely repetitive larger input still
  shows non-zero byte comparisons from Kasai LCP construction */
  Bytes zeros(200000, 0);
  std::vector<size_t> zends(1, zeros.size());
  LZSettings zst = {2, 32768, 3, 258, 1};
  target::lodepng_lz77test_byte_comparisons = 0;
  target::lodepng_lz77test_candidate_checks = 0;
  std::vector<unsigned> ztok;
  CHECK(tokenStreams(zeros, zends, zst, true, &ztok));
  CHECK(target::lodepng_lz77test_byte_comparisons > 0);
  CHECK(target::lodepng_lz77test_candidate_checks > 0);
  CHECK(tokensEqual(zeros, zends, zst));
  return 1;
}

/* ---------------------------------------------------------------- PNG paths */

struct Rgba { unsigned char r, g, b, a; };

static bool encodeDecodePng(const Bytes& raw, unsigned w, unsigned h,
                            int paletteMode, int greyMode, int adam7) {
  target::LodePNGState ts;
  stock::LodePNGState ss;
  target::lodepng_state_init(&ts);
  stock::lodepng_state_init(&ss);

  ts.encoder.auto_convert = ss.encoder.auto_convert = 0;
  ts.encoder.zlibsettings.windowsize = ss.encoder.zlibsettings.windowsize = 32768;
  ts.encoder.zlibsettings.nicematch = ss.encoder.zlibsettings.nicematch = 258;
  ts.info_png.interlace_method = ss.info_png.interlace_method = adam7 ? 1 : 0;

  if(paletteMode) {
    for(unsigned i = 0; i < 16; ++i) {
      target::lodepng_palette_add(&ts.info_png.color, (unsigned char)(i * 16),
                                  (unsigned char)(255 - i * 16), 0, 255);
      stock::lodepng_palette_add(&ss.info_png.color, (unsigned char)(i * 16),
                                 (unsigned char)(255 - i * 16), 0, 255);
    }
    ts.info_png.color.colortype = target::LCT_PALETTE;
    ss.info_png.color.colortype = stock::LCT_PALETTE;
    ts.info_png.color.bitdepth = ss.info_png.color.bitdepth = 4;
    ts.info_raw.colortype = target::LCT_PALETTE;
    ss.info_raw.colortype = stock::LCT_PALETTE;
    ts.info_raw.bitdepth = ss.info_raw.bitdepth = 4;
  } else if(greyMode) {
    ts.info_png.color.colortype = target::LCT_GREY;
    ss.info_png.color.colortype = stock::LCT_GREY;
    ts.info_png.color.bitdepth = ss.info_png.color.bitdepth = (unsigned)greyMode;
    ts.info_raw.colortype = target::LCT_GREY;
    ss.info_raw.colortype = stock::LCT_GREY;
    ts.info_raw.bitdepth = ss.info_raw.bitdepth = (unsigned)greyMode;
  } else {
    ts.encoder.auto_convert = ss.encoder.auto_convert = 1;
    ts.info_png.interlace_method = ss.info_png.interlace_method = adam7 ? 1 : 0;
  }

  unsigned char* tpng = 0; size_t tsize = 0;
  unsigned char* spng = 0; size_t ssize = 0;
  unsigned e1 = target::lodepng_encode(&tpng, &tsize, raw.data(), w, h, &ts);
  unsigned e2 = stock::lodepng_encode(&spng, &ssize, raw.data(), w, h, &ss);
  bool ok = (!e1 && !e2 && tsize == ssize && !memcmp(tpng, spng, tsize));
  if(!ok) std::fprintf(stderr, "png encode mismatch e=%u/%u sizes=%zu/%zu\n",
                       e1, e2, tsize, ssize);

  if(ok) {
    /* decode back to 32-bit RGBA regardless of storage color type */
    unsigned char* dec = 0; unsigned dw = 0, dh = 0;
    unsigned e3 = target::lodepng_decode32(&dec, &dw, &dh, tpng, tsize);
    ok = !e3 && dw == w && dh == h;
    if(ok) {
      Bytes expect(w * h * 4);
      if(paletteMode || greyMode) {
        unsigned bd = paletteMode ? 4u : (unsigned)greyMode;
        unsigned maxv = (1u << bd) - 1u;
        /* lodepng raw buffers pack all w*h samples bit-continuously, MSB first */
        for(size_t p = 0; p < (size_t)w * h; ++p) {
          size_t bit = p * bd;
          unsigned v = (unsigned)((raw[bit / 8] >> (8 - bd - (bit % 8))) & maxv);
          if(paletteMode) {
            expect[p*4+0] = (unsigned char)(v * 16);
            expect[p*4+1] = (unsigned char)(255 - v * 16);
            expect[p*4+2] = 0;
          } else {
            unsigned char g = (unsigned char)(v * 255u / maxv);
            expect[p*4+0] = expect[p*4+1] = expect[p*4+2] = g;
          }
          expect[p*4+3] = 255;
        }
      } else {
        expect = raw; /* already RGBA */
      }
      ok = !memcmp(dec, expect.data(), expect.size());
    }
    /* metadata check */
    target::LodePNGState insp;
    target::lodepng_state_init(&insp);
    unsigned iw = 0, ih = 0;
    if(ok) ok = !target::lodepng_inspect(&iw, &ih, &insp, tpng, tsize);
    if(ok) {
      ok = (iw == w && ih == h &&
            insp.info_png.interlace_method == (unsigned)(adam7 ? 1 : 0));
      if(paletteMode) ok = ok && insp.info_png.color.colortype == target::LCT_PALETTE
                            && insp.info_png.color.bitdepth == 4;
      if(greyMode) ok = ok && insp.info_png.color.colortype == target::LCT_GREY
                            && insp.info_png.color.bitdepth == (unsigned)greyMode;
    }
    target::lodepng_state_cleanup(&insp);
    free(dec);
  }
  free(tpng); free(spng);
  target::lodepng_state_cleanup(&ts);
  stock::lodepng_state_cleanup(&ss);
  return ok;
}

/* 5. png-path: RGBA, low-bit-depth grayscale, palette and Adam7. */
static int run_png_path() {
  const unsigned W = 37, H = 29;
  Bytes rgba(W * H * 4);
  for(size_t i = 0; i < W * H; ++i) {
    rgba[i * 4 + 0] = (unsigned char)((i * 17) % 256);
    rgba[i * 4 + 1] = (unsigned char)((i * 31 + 3) % 256);
    rgba[i * 4 + 2] = (unsigned char)((i / 7) % 256);
    rgba[i * 4 + 3] = (unsigned char)(128 + (i % 128));
  }
  CHECK(encodeDecodePng(rgba, W, H, 0, 0, 0));       /* RGBA */
  CHECK(encodeDecodePng(rgba, W, H, 0, 0, 1));       /* RGBA + Adam7 */

  /* low bit depth grayscale: 1, 2, 4 bits, packed bit-continuously MSB first */
  for(unsigned bd = 1; bd <= 4; bd <<= 1) {
    unsigned maxv = (1u << bd) - 1u;
    size_t total = (size_t)W * H;
    Bytes grey((total * bd + 7u) / 8u, 0);
    for(size_t p = 0; p < total; ++p) {
      unsigned v = (unsigned)(p & maxv);
      size_t bit = p * bd;
      grey[bit / 8] |= (unsigned char)(v << (8 - bd - (bit % 8)));
    }
    CHECK(encodeDecodePng(grey, W, H, 0, (int)bd, 0));
  }
  /* palette image: 4-bit indices packed bit-continuously, two per byte */
  {
    unsigned bd = 4, maxv = 15;
    size_t total = (size_t)W * H;
    Bytes indexed((total * bd + 7u) / 8u, 0);
    for(size_t p = 0; p < total; ++p) {
      unsigned v = (unsigned)(p & maxv);
      size_t bit = p * bd;
      indexed[bit / 8] |= (unsigned char)(v << (8 - bd - (bit % 8)));
    }
    CHECK(encodeDecodePng(indexed, W, H, 1, 0, 0));
    CHECK(encodeDecodePng(indexed, W, H, 1, 0, 1));   /* palette + Adam7 */
  }
  return 1;
}

/* 6. c-compatibility: production as strict C, real compress/decompress run. */
static int run_c_compatibility() {
  if(access("lodepng.cpp", R_OK) != 0 || access("lodepng.h", R_OK) != 0) {
    std::fprintf(stderr, "must run from repository root\n");
    return 0;
  }
  static unsigned counter = 0;
  char csrc[512], bin[512], cmd[2048];
  snprintf(csrc, sizeof(csrc), "%s/lz77_c_%d_%u.c",
           tmpDir(), (int)getpid(), counter);
  snprintf(bin, sizeof(bin), "%s/lz77_c_%d_%u.bin",
           tmpDir(), (int)getpid(), counter);
  ++counter;
  {
    std::ofstream f(csrc);
    if(!f) return 0;
    f <<
      "#include \"lodepng.h\"\n"
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "int main(void) {\n"
      "  unsigned char in[4096]; size_t i;\n"
      "  unsigned char *z=0,*d=0; size_t nz=0,nd=0;\n"
      "  LodePNGCompressSettings s;\n"
      "  for(i=0;i<sizeof(in);++i) in[i]=(unsigned char)(i%19?65:(unsigned char)(i*7));\n"
      "  lodepng_compress_settings_init(&s);\n"
      "  s.windowsize=32768; s.nicematch=258;\n"
      "  if(lodepng_zlib_compress(&z,&nz,in,sizeof(in),&s)) return 1;\n"
      "  if(lodepng_zlib_decompress(&d,&nd,z,nz,&lodepng_default_decompress_settings)) return 2;\n"
      "  if(nd!=sizeof(in)||memcmp(d,in,nd)) return 3;\n"
      "  free(z);free(d);z=0;d=0;nz=nd=0;\n"
      "  s.use_lz77=0;\n"
      "  if(lodepng_zlib_compress(&z,&nz,in,sizeof(in),&s)) return 4;\n"
      "  if(lodepng_zlib_decompress(&d,&nd,z,nz,&lodepng_default_decompress_settings)) return 5;\n"
      "  if(nd!=sizeof(in)||memcmp(d,in,nd)) return 6;\n"
      "  free(z);free(d);z=0;d=0;nz=nd=0;\n"
      "  s.use_lz77=1; s.btype=0;\n"
      "  if(lodepng_zlib_compress(&z,&nz,in,sizeof(in),&s)) return 7;\n"
      "  if(lodepng_zlib_decompress(&d,&nd,z,nz,&lodepng_default_decompress_settings)) return 8;\n"
      "  if(nd!=sizeof(in)||memcmp(d,in,nd)) return 9;\n"
      "  free(z);free(d);\n"
      "  return 0;\n"
      "}\n";
  }
  /* -x c: lodepng.cpp is C-clean despite its .cpp suffix */
  snprintf(cmd, sizeof(cmd),
           "cc -std=c89 -ansi -pedantic -O2 -Wno-unused-function -I. -x c \"%s\" lodepng.cpp -o \"%s\" && \"%s\"",
           csrc, bin, bin);
  int rc = system(cmd);
  remove(csrc);
  remove(bin);
  CHECK(rc == 0);
  return 1;
}

} /* namespace */

/* External linkage so verification/driver_main.cpp can drive these. */
test_case cases[] = {
  {"legacy-stream", run_legacy_stream},
  {"lazy-window", run_lazy_window},
  {"block-history", run_block_history},
  {"index-work", run_index_work},
  {"png-path", run_png_path},
  {"c-compatibility", run_c_compatibility}
};
size_t case_count = sizeof(cases) / sizeof(cases[0]);