#include "support/check.hpp"

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <iostream>

/* The check header includes lodepng.h inside an extern "C" block. */

namespace {

typedef std::vector<unsigned char> Bytes;

struct Collector {
  unsigned w = 0;
  unsigned h = 0;
  unsigned calls = 0;
  bool bad = false;
  std::vector<unsigned> ys;
  std::vector<Bytes> rows;
};

extern "C" void row_stream_test_callback(void* user, unsigned w, unsigned h,
                                         unsigned y, const unsigned char* rgba) {
  Collector* c = static_cast<Collector*>(user);
  ++c->calls;
  if(w == 0 || h == 0 || y >= h || y != c->rows.size()) c->bad = true;
  size_t n = static_cast<size_t>(w) * 4u;
  c->w = w;
  c->h = h;
  c->ys.push_back(y);
  c->rows.push_back(Bytes(rgba, rgba + n));
}

LodePNGRowDecoder* makeCollector(Collector& c) {
  return lodepng_row_decoder_create(row_stream_test_callback, &c);
}

Bytes flattenRows(const Collector& c) {
  Bytes out;
  for(size_t i = 0; i < c.rows.size(); ++i) {
    out.insert(out.end(), c.rows[i].begin(), c.rows[i].end());
  }
  return out;
}

void verifyCollector(const Collector& c, unsigned w, unsigned h,
                     const Bytes& expected) {
  UNIT_CHECK(!c.bad);
  UNIT_CHECK(c.w == w && c.h == h);
  UNIT_CHECK(c.rows.size() == h);
  UNIT_CHECK(c.ys.size() == h);
  Bytes got = flattenRows(c);
  UNIT_CHECK(got.size() == static_cast<size_t>(w) * h * 4u);
  UNIT_CHECK(expected.size() == got.size());
  if(expected.size() == got.size()) {
    UNIT_CHECK(std::memcmp(expected.data(), got.data(), expected.size()) == 0);
  }
  for(unsigned y = 0; y < h; ++y) UNIT_CHECK(c.ys[y] == y);
}

Bytes makePixels(unsigned w, unsigned h, unsigned seed = 0) {
  Bytes px(static_cast<size_t>(w) * h * 4u);
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 4u;
      px[i + 0] = static_cast<unsigned char>((x * 37u + y * 17u + seed) & 255u);
      px[i + 1] = static_cast<unsigned char>((x * 11u + y * 53u + seed * 3u) & 255u);
      px[i + 2] = static_cast<unsigned char>((x * 71u + y * 7u + seed * 5u) & 255u);
      px[i + 3] = static_cast<unsigned char>(255u - ((x + y + seed) % 9u));
    }
  }
  return px;
}

Bytes encodeRGBA(unsigned w, unsigned h, const Bytes& px,
                 int btype = -1, const unsigned char* filters = 0) {
  LodePNGState state;
  unsigned char* png = 0;
  size_t png_size = 0;
  lodepng_state_init(&state);
  state.encoder.auto_convert = 0;
  state.info_png.color.colortype = LCT_RGBA;
  state.info_png.color.bitdepth = 8;
  state.info_raw.colortype = LCT_RGBA;
  state.info_raw.bitdepth = 8;
  if(btype >= 0) state.encoder.zlibsettings.btype = static_cast<unsigned>(btype);
  if(filters) {
    state.encoder.filter_palette_zero = 0;
    state.encoder.filter_strategy = LFS_PREDEFINED;
    state.encoder.predefined_filters = const_cast<unsigned char*>(filters);
  }
  unsigned error = lodepng_encode(&png, &png_size, px.data(), w, h, &state);
  UNIT_CHECK(error == 0);
  Bytes out(png, png + png_size);
  lodepng_free(png);
  lodepng_state_cleanup(&state);
  return out;
}

Bytes decodeOld32(const Bytes& png, unsigned& w, unsigned& h) {
  unsigned char* out = 0;
  w = h = 0;
  unsigned error = lodepng_decode32(&out, &w, &h, png.data(), png.size());
  UNIT_CHECK(error == 0);
  UNIT_CHECK(out != 0);
  Bytes px(out, out + static_cast<size_t>(w) * h * 4u);
  lodepng_free(out);
  return px;
}

void checkAllCuts(const Bytes& png, unsigned w, unsigned h, const Bytes& expected) {
  UNIT_CHECK(!png.empty());
  for(size_t cut = 0; cut <= png.size(); ++cut) {
    Collector c;
    LodePNGRowDecoder* d = makeCollector(c);
    UNIT_CHECK(d != 0);
    unsigned error = lodepng_row_decoder_feed(d, png.data(), cut, 0);
    UNIT_CHECK(error == 0);
    error = lodepng_row_decoder_feed(d, png.data() + cut, png.size() - cut, 1);
    UNIT_CHECK(error == 0);
    UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
    verifyCollector(c, w, h, expected);

    UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == 0);
    unsigned char trailing = 0;
    UNIT_CHECK(lodepng_row_decoder_feed(d, &trailing, 1, 0) != 0);
    UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) != 0);
    lodepng_row_decoder_destroy(d);
  }
}

void checkBytewiseAndReused(const Bytes& png, unsigned w, unsigned h,
                            const Bytes& expected) {
  {
    Collector c;
    LodePNGRowDecoder* d = makeCollector(c);
    for(size_t i = 0; i < png.size(); ++i) {
      unsigned error = lodepng_row_decoder_feed(d, png.data() + i, 1,
                                                i + 1 == png.size());
      UNIT_CHECK(error == 0);
    }
    UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
    verifyCollector(c, w, h, expected);
    lodepng_row_decoder_destroy(d);
  }

  {
    Bytes scratch(19);
    Collector c;
    LodePNGRowDecoder* d = makeCollector(c);
    size_t pos = 0;
    while(pos < png.size()) {
      size_t amount = png.size() - pos;
      if(amount > 17) amount = 17;
      std::memcpy(scratch.data(), png.data() + pos, amount);
      unsigned error = lodepng_row_decoder_feed(d, scratch.data(), amount,
                                                pos + amount == png.size());
      UNIT_CHECK(error == 0);
      for(size_t j = 0; j < scratch.size(); ++j) scratch[j] = 0xaa;
      pos += amount;
    }
    UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
    verifyCollector(c, w, h, expected);
    lodepng_row_decoder_destroy(d);
  }
}

void put32(Bytes& b, size_t offset, unsigned value) {
  b[offset + 0] = static_cast<unsigned char>((value >> 24) & 255u);
  b[offset + 1] = static_cast<unsigned char>((value >> 16) & 255u);
  b[offset + 2] = static_cast<unsigned char>((value >> 8) & 255u);
  b[offset + 3] = static_cast<unsigned char>(value & 255u);
}

unsigned get32(const unsigned char* p) {
  return (static_cast<unsigned>(p[0]) << 24) |
         (static_cast<unsigned>(p[1]) << 16) |
         (static_cast<unsigned>(p[2]) << 8) |
         static_cast<unsigned>(p[3]);
}

Bytes makeChunk(const char type[5], const Bytes& data) {
  Bytes chunk(12 + data.size());
  put32(chunk, 0, static_cast<unsigned>(data.size()));
  std::memcpy(chunk.data() + 4, type, 4);
  if(!data.empty()) std::memcpy(chunk.data() + 8, data.data(), data.size());
  unsigned crc = lodepng_crc32(chunk.data() + 4, data.size() + 4);
  put32(chunk, 8 + data.size(), crc);
  return chunk;
}

Bytes makePNGFromZlib(unsigned w, unsigned h, const Bytes& zlib) {
  Bytes ihdr(13, 0);
  put32(ihdr, 0, w);
  put32(ihdr, 4, h);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  Bytes png;
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  png.insert(png.end(), sig, sig + 8);
  Bytes c = makeChunk("IHDR", ihdr);
  png.insert(png.end(), c.begin(), c.end());
  c = makeChunk("IDAT", zlib);
  png.insert(png.end(), c.begin(), c.end());
  c = makeChunk("IEND", Bytes());
  png.insert(png.end(), c.begin(), c.end());
  return png;
}

unsigned adler32Bytes(const Bytes& data) {
  unsigned s1 = 1, s2 = 0;
  for(size_t i = 0; i < data.size(); ++i) {
    s1 = (s1 + data[i]) % 65521u;
    s2 = (s2 + s1) % 65521u;
  }
  return (s2 << 16) | s1;
}

Bytes zlibWrap(const Bytes& deflate, const Bytes& raw) {
  Bytes z;
  z.push_back(0x78);
  z.push_back(0x01);
  z.insert(z.end(), deflate.begin(), deflate.end());
  unsigned adler = adler32Bytes(raw);
  z.push_back(static_cast<unsigned char>((adler >> 24) & 255u));
  z.push_back(static_cast<unsigned char>((adler >> 16) & 255u));
  z.push_back(static_cast<unsigned char>((adler >> 8) & 255u));
  z.push_back(static_cast<unsigned char>(adler & 255u));
  return z;
}

struct BitWriter {
  Bytes bytes;
  unsigned acc = 0;
  unsigned n = 0;

  void put(unsigned value, unsigned bits) {
    for(unsigned i = 0; i < bits; ++i) {
      acc |= ((value >> i) & 1u) << n;
      ++n;
      if(n == 8) {
        bytes.push_back(static_cast<unsigned char>(acc & 255u));
        acc = 0;
        n = 0;
      }
    }
  }

  void huff(unsigned code, unsigned bits) {
    for(int i = static_cast<int>(bits) - 1; i >= 0; --i) {
      put((code >> i) & 1u, 1);
    }
  }

  void align() {
    if(n) {
      bytes.push_back(static_cast<unsigned char>(acc & 255u));
      acc = 0;
      n = 0;
    }
  }

  Bytes finish() {
    align();
    return bytes;
  }
};

std::vector<unsigned> canonicalCodes(const std::vector<unsigned>& lengths) {
  unsigned maxbits = 0;
  for(unsigned n : lengths) if(n > maxbits) maxbits = n;
  std::vector<unsigned> blcount(maxbits + 1, 0);
  std::vector<unsigned> nextcode(maxbits + 1, 0);
  std::vector<unsigned> codes(lengths.size(), 0);
  for(size_t i = 0; i < lengths.size(); ++i) ++blcount[lengths[i]];
  for(unsigned bits = 1; bits <= maxbits; ++bits) {
    nextcode[bits] = (nextcode[bits - 1] + blcount[bits - 1]) << 1u;
  }
  for(size_t i = 0; i < lengths.size(); ++i) {
    if(lengths[i]) codes[i] = nextcode[lengths[i]]++;
  }
  return codes;
}

unsigned fixedLitCode(unsigned sym, unsigned& bits) {
  if(sym <= 143) { bits = 8; return 0x30u + sym; }
  if(sym <= 255) { bits = 9; return 0x190u + (sym - 144u); }
  if(sym <= 279) { bits = 7; return sym - 256u; }
  bits = 8;
  return 0xc0u + (sym - 280u);
}

void emitFixedLit(BitWriter& bw, unsigned sym) {
  unsigned bits, code = fixedLitCode(sym, bits);
  bw.huff(code, bits);
}

void emitFixedDist(BitWriter& bw, unsigned distCode) {
  bw.huff(distCode, 5);
}

Bytes storedDeflate(const Bytes& first, const Bytes& second) {
  BitWriter bw;
  bw.put(0, 1); /* not final */
  bw.put(0, 2); /* stored */
  bw.align();
  unsigned len1 = static_cast<unsigned>(first.size());
  bw.put(len1 & 255u, 8);
  bw.put((len1 >> 8) & 255u, 8);
  unsigned nlen1 = (~len1) & 65535u;
  bw.put(nlen1 & 255u, 8);
  bw.put((nlen1 >> 8) & 255u, 8);
  bw.bytes.insert(bw.bytes.end(), first.begin(), first.end());

  bw.put(1, 1); /* final */
  bw.put(0, 2);
  bw.align();
  unsigned len2 = static_cast<unsigned>(second.size());
  bw.put(len2 & 255u, 8);
  bw.put((len2 >> 8) & 255u, 8);
  unsigned nlen2 = (~len2) & 65535u;
  bw.put(nlen2 & 255u, 8);
  bw.put((nlen2 >> 8) & 255u, 8);
  bw.bytes.insert(bw.bytes.end(), second.begin(), second.end());
  return bw.finish();
}

void addCopy(Bytes& expected, unsigned length, unsigned distance) {
  for(unsigned i = 0; i < length; ++i) {
    UNIT_CHECK(expected.size() >= distance);
    expected.push_back(expected[expected.size() - distance]);
  }
}

Bytes makeFixedWindowStream(Bytes& rawExpected) {
  BitWriter bw;
  rawExpected.clear();

  bw.put(0, 1); /* first block is not final */
  bw.put(1, 2); /* fixed Huffman */

  emitFixedLit(bw, 0);
  emitFixedLit(bw, 0);
  emitFixedLit(bw, 0);
  rawExpected.insert(rawExpected.end(), 3, 0);

  emitFixedLit(bw, 258); /* length 4 */
  emitFixedDist(bw, 2);  /* distance 3: distance < length */
  addCopy(rawExpected, 4, 3);

  for(int block = 0; block < 2; ++block) {
    unsigned copies = block == 0 ? 65u : 66u;
    for(unsigned i = 0; i < copies; ++i) {
      emitFixedLit(bw, 285); /* length 258 */
      emitFixedDist(bw, 0);  /* distance 1 */
      addCopy(rawExpected, 258, 1);
    }
    if(block == 0) {
      emitFixedLit(bw, 256); /* end first block */
      bw.put(1, 1);         /* second/final block */
      bw.put(1, 2); /* fixed Huffman */
    }
  }

  for(int i = 0; i < 2; ++i) {
    emitFixedLit(bw, 258); /* length 4 */
    emitFixedDist(bw, 0);
    addCopy(rawExpected, 4, 1);
  }
  for(int i = 0; i < 37; ++i) {
    emitFixedLit(bw, 257); /* length 3 */
    emitFixedDist(bw, 0);
    addCopy(rawExpected, 3, 1);
  }

  emitFixedLit(bw, 256);
  return bw.finish();
}

Bytes makeDynamicRepeatStream() {
  static const unsigned order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  std::vector<unsigned> clLen(19, 0);
  clLen[1] = clLen[2] = clLen[3] = clLen[16] = 3;
  clLen[17] = clLen[18] = 2;
  std::vector<unsigned> clCode = canonicalCodes(clLen);

  std::vector<unsigned> llLen(288, 0);
  llLen[0] = 2;
  llLen[65] = llLen[66] = llLen[67] = llLen[68] = 3;
  llLen[256] = 2;
  std::vector<unsigned> llCode = canonicalCodes(llLen);
  std::vector<unsigned> dLen(32, 0);
  dLen[0] = 1;
  std::vector<unsigned> dCode = canonicalCodes(dLen);

  BitWriter bw;
  bw.put(1, 1); /* final */
  bw.put(2, 2); /* dynamic */
  bw.put(0, 5); /* HLIT = 257 */
  bw.put(0, 5); /* HDIST = 1 */
  bw.put(15, 4); /* HCLEN = 19 */
  for(unsigned i = 0; i < 19; ++i) bw.put(clLen[order[i]], 3);

  bw.huff(clCode[2], clLen[2]);  /* ll[0] has code length 2 */
  bw.huff(clCode[18], clLen[18]);
  bw.put(53, 7);         /* zero run 11+53=64 */
  bw.huff(clCode[3], clLen[3]); /* ll[65]=3 */
  bw.huff(clCode[16], clLen[16]);
  bw.put(0, 2);          /* repeat previous 3 times */
  bw.huff(clCode[17], clLen[17]);
  bw.put(0, 3);          /* three zeros */
  bw.huff(clCode[18], clLen[18]);
  bw.put(127, 7);        /* 138 zeros */
  bw.huff(clCode[18], clLen[18]);
  bw.put(35, 7);         /* 46 zeros */
  bw.huff(clCode[2], clLen[2]); /* end symbol has length 2 */
  bw.huff(clCode[1], clLen[1]); /* one distance code of length one */

  bw.huff(llCode[0], llLen[0]);
  bw.huff(llCode[65], llLen[65]);
  bw.huff(llCode[66], llLen[66]);
  bw.huff(llCode[67], llLen[67]);
  bw.huff(llCode[68], llLen[68]);
  bw.huff(llCode[256], llLen[256]);
  (void)dCode;
  return bw.finish();
}

Bytes inflateWhole(const Bytes& deflate) {
  unsigned char* out = 0;
  size_t outsize = 0;
  unsigned error = lodepng_inflate(&out, &outsize, deflate.data(), deflate.size(),
                                   &lodepng_default_decompress_settings);
  UNIT_CHECK(error == 0);
  UNIT_CHECK(out != 0 || outsize == 0);
  Bytes result(out, out + outsize);
  lodepng_free(out);
  return result;
}

struct ChunkInfo {
  size_t pos;
  size_t len;
  unsigned char type[5];
};

std::vector<ChunkInfo> findChunks(const Bytes& png) {
  std::vector<ChunkInfo> chunks;
  size_t pos = 8;
  while(pos + 8 <= png.size()) {
    ChunkInfo ch;
    ch.pos = pos;
    ch.len = get32(png.data() + pos);
    std::memcpy(ch.type, png.data() + pos + 4, 4);
    ch.type[4] = 0;
    chunks.push_back(ch);
    if(pos + 12 + ch.len > png.size()) break;
    pos += 12 + ch.len;
  }
  return chunks;
}

Bytes idatPayload(const Bytes& png) {
  Bytes z;
  std::vector<ChunkInfo> chunks = findChunks(png);
  for(size_t i = 0; i < chunks.size(); ++i) {
    if(std::memcmp(chunks[i].type, "IDAT", 4) == 0) {
      z.insert(z.end(), png.begin() + chunks[i].pos + 8,
               png.begin() + chunks[i].pos + 8 + chunks[i].len);
    }
  }
  return z;
}

extern "C" unsigned test_custom_inflate(unsigned char** out, size_t* outsize,
                                        const unsigned char* in, size_t insize,
                                        const LodePNGDecompressSettings* settings) {
  (void)settings;
  static int calls = 0;
  ++calls;
  return lodepng_inflate(out, outsize, in, insize,
                          &lodepng_default_decompress_settings);
}

extern "C" unsigned test_custom_zlib(unsigned char** out, size_t* outsize,
                                    const unsigned char* in, size_t insize,
                                    const LodePNGDecompressSettings* settings) {
  (void)settings;
  static int calls = 0;
  ++calls;
  return lodepng_zlib_decompress(out, outsize, in, insize,
                                 &lodepng_default_decompress_settings);
}

/* ------------------------- deflate group ------------------------- */

void case_deflate_stored() {
  Bytes first(137), second(913);
  for(size_t i = 0; i < first.size(); ++i) first[i] = static_cast<unsigned char>(i * 3 + 1);
  for(size_t i = 0; i < second.size(); ++i) second[i] = static_cast<unsigned char>(i * 5 + 7);
  Bytes expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  Bytes def = storedDeflate(first, second);
  Bytes got = inflateWhole(def);
  UNIT_CHECK(got == expected);
}

void case_deflate_fixed_window() {
  Bytes raw;
  Bytes def = makeFixedWindowStream(raw);
  UNIT_CHECK(raw.size() == 33924);
  Bytes got = inflateWhole(def);
  UNIT_CHECK(got.size() == raw.size());
  UNIT_CHECK(got == raw);

  Bytes z = zlibWrap(def, raw);
  Bytes png = makePNGFromZlib(8, 1028, z);
  Bytes expected(8u * 1028u * 4u, 0);
  checkAllCuts(png, 8, 1028, expected);
}

void case_deflate_dynamic_repeats() {
  Bytes def = makeDynamicRepeatStream();
  Bytes expected;
  expected.push_back(0);
  expected.push_back(65);
  expected.push_back(66);
  expected.push_back(67);
  expected.push_back(68);
  Bytes got = inflateWhole(def);
  UNIT_CHECK(got == expected);

  Bytes z = zlibWrap(def, expected);
  Bytes png = makePNGFromZlib(1, 1, z);
  Bytes rgba;
  rgba.push_back(65);
  rgba.push_back(66);
  rgba.push_back(67);
  rgba.push_back(68);
  checkAllCuts(png, 1, 1, rgba);
  checkBytewiseAndReused(png, 1, 1, rgba);
}

void case_deflate_encoded_block_types() {
  unsigned w = 29, h = 11;
  Bytes px = makePixels(w, h, 4);
  for(int btype = 0; btype <= 2; ++btype) {
    Bytes png = encodeRGBA(w, h, px, btype);
    unsigned rw, rh;
    Bytes expected = decodeOld32(png, rw, rh);
    UNIT_CHECK(rw == w && rh == h && expected == px);
    Collector c;
    LodePNGRowDecoder* d = makeCollector(c);
    for(size_t i = 0; i < png.size(); i += 23) {
      size_t n = png.size() - i;
      if(n > 23) n = 23;
      UNIT_CHECK(lodepng_row_decoder_feed(d, png.data() + i, n,
                                          i + n == png.size()) == 0);
    }
    UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
    verifyCollector(c, w, h, expected);
    lodepng_row_decoder_destroy(d);
    checkAllCuts(png, w, h, expected);
  }
}

void case_deflate_custom_settings() {
  unsigned w = 17, h = 5;
  Bytes px = makePixels(w, h, 9);
  Bytes png = encodeRGBA(w, h, px, 2);
  Bytes z = idatPayload(png);
  UNIT_CHECK(z.size() > 6);

  LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
  settings.custom_inflate = test_custom_inflate;
  unsigned char* out = 0;
  size_t outsize = 0;
  UNIT_CHECK(lodepng_zlib_decompress(&out, &outsize, z.data(), z.size(), &settings) == 0);
  UNIT_CHECK(outsize == static_cast<size_t>(h) * (w * 4u + 1u));
  lodepng_free(out);
  out = 0;
  outsize = 0;

  LodePNGState state;
  lodepng_state_init(&state);
  state.decoder.zlibsettings.custom_zlib = test_custom_zlib;
  unsigned char* decoded = 0;
  unsigned rw = 0, rh = 0;
  unsigned error = lodepng_decode(&decoded, &rw, &rh, &state, png.data(), png.size());
  UNIT_CHECK(error == 0);
  UNIT_CHECK(rw == w && rh == h);
  lodepng_free(decoded);
  lodepng_state_cleanup(&state);

  Bytes badAdlerZ = z;
  badAdlerZ[badAdlerZ.size() - 1] ^= 0x01;
  UNIT_CHECK(lodepng_zlib_decompress(&out, &outsize, badAdlerZ.data(),
                                     badAdlerZ.size(),
                                     &lodepng_default_decompress_settings) != 0);
  settings = lodepng_default_decompress_settings;
  settings.ignore_adler32 = 1;
  UNIT_CHECK(lodepng_zlib_decompress(&out, &outsize, badAdlerZ.data(),
                                     badAdlerZ.size(), &settings) == 0);
  lodepng_free(out);
}

/* -------------------------- rows group -------------------------- */

void case_rows_known_filters() {
  unsigned w = 31, h = 10;
  Bytes px = makePixels(w, h, 12);
  std::vector<unsigned char> filters(h);
  for(unsigned y = 0; y < h; ++y) filters[y] = static_cast<unsigned char>(y % 5u);
  Bytes png = encodeRGBA(w, h, px, -1, filters.data());
  unsigned rw, rh;
  Bytes expected = decodeOld32(png, rw, rh);
  UNIT_CHECK(expected == px);

  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  size_t pos = 0;
  while(pos < png.size()) {
    size_t n = png.size() - pos;
    if(n > 37) n = 37;
    UNIT_CHECK(lodepng_row_decoder_feed(d, png.data() + pos, n,
                                        pos + n == png.size()) == 0);
    pos += n;
  }
  UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
  verifyCollector(c, w, h, expected);
  lodepng_row_decoder_destroy(d);
}

void case_rows_each_filter() {
  unsigned w = 13, h = 6;
  Bytes px = makePixels(w, h, 21);
  for(unsigned f = 0; f < 5; ++f) {
    std::vector<unsigned char> filters(h, static_cast<unsigned char>(f));
    Bytes png = encodeRGBA(w, h, px, 0, filters.data());
    unsigned rw, rh;
    Bytes expected = decodeOld32(png, rw, rh);
    UNIT_CHECK(expected == px);
    checkBytewiseAndReused(png, w, h, expected);
  }
}

void case_rows_single_cuts() {
  unsigned w = 19, h = 7;
  Bytes px = makePixels(w, h, 33);
  std::vector<unsigned char> filters(h);
  for(unsigned y = 0; y < h; ++y) filters[y] = static_cast<unsigned char>((y * 2u + 1u) % 5u);
  Bytes png = encodeRGBA(w, h, px, 2, filters.data());
  unsigned rw, rh;
  Bytes expected = decodeOld32(png, rw, rh);
  checkAllCuts(png, w, h, expected);
}

void case_rows_bytewise_reuse() {
  unsigned w = 23, h = 9;
  Bytes px = makePixels(w, h, 44);
  Bytes png = encodeRGBA(w, h, px, 1);
  unsigned rw, rh;
  Bytes expected = decodeOld32(png, rw, rh);
  checkBytewiseAndReused(png, w, h, expected);
}

void case_rows_early_callback() {
  unsigned w = 96, h = 160;
  Bytes px(static_cast<size_t>(w) * h * 4u);
  uint32_t state = 0x12345678u;
  for(size_t i = 0; i < px.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    px[i] = static_cast<unsigned char>(state >> 16);
  }
  for(size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  Bytes png = encodeRGBA(w, h, px, 2);
  Bytes expected;
  unsigned rw, rh;
  expected = decodeOld32(png, rw, rh);

  std::vector<ChunkInfo> chunks = findChunks(png);
  size_t idatData = 0, idatLen = 0;
  for(size_t i = 0; i < chunks.size(); ++i) {
    if(std::memcmp(chunks[i].type, "IDAT", 4) == 0) {
      idatData = chunks[i].pos + 8;
      idatLen = chunks[i].len;
      break;
    }
  }
  UNIT_CHECK(idatLen > 1000);
  size_t prefixEnd = idatData + 4096;
  UNIT_CHECK(prefixEnd < idatData + idatLen);

  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, png.data(), prefixEnd, 0) == 0);
  UNIT_CHECK(c.calls >= 8);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);

  UNIT_CHECK(lodepng_row_decoder_feed(d, png.data() + prefixEnd,
                                      png.size() - prefixEnd, 1) == 0);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
  verifyCollector(c, w, h, expected);
  lodepng_row_decoder_destroy(d);
}

size_t memoryPeakForHeight(unsigned w, unsigned h, const Bytes& px) {
  std::vector<unsigned char> filters(h, 0);
  Bytes png = encodeRGBA(w, h, px, 0, filters.data());
  Collector c;
  size_t before = unit_alloc_current();
  LodePNGRowDecoder* d = makeCollector(c);
  size_t base = unit_alloc_current();
  unit_alloc_reset_peak();
  for(size_t i = 0; i < png.size(); i += 101) {
    size_t n = png.size() - i;
    if(n > 101) n = 101;
    UNIT_CHECK(lodepng_row_decoder_feed(d, png.data() + i, n,
                                        i + n == png.size()) == 0);
  }
  UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
  size_t peak = unit_alloc_peak();
  UNIT_CHECK(!c.bad && c.rows.size() == h);
  lodepng_row_decoder_destroy(d);
  UNIT_CHECK(unit_alloc_current() == before);
  return peak - base;
}

void case_rows_memory_shape() {
  unsigned w = 48;
  Bytes px1 = makePixels(w, 1, 1);
  Bytes px16 = makePixels(w, 16, 2);
  Bytes px128 = makePixels(w, 128, 3);
  size_t p1 = memoryPeakForHeight(w, 1, px1);
  size_t p16 = memoryPeakForHeight(w, 16, px16);
  size_t p128 = memoryPeakForHeight(w, 128, px128);
  UNIT_CHECK(p1 > 0);
  UNIT_CHECK(p16 == p1);
  UNIT_CHECK(p128 == p1);

  Bytes px8 = makePixels(w, 8, 4);
  Bytes png = encodeRGBA(w, 8, px8, 0);
  static const size_t ancLengths[2] = {1024, 65536};
  size_t peaks[2];
  for(int variant = 0; variant < 2; ++variant) {
    Bytes anc(ancLengths[variant], 0x5a);
    Bytes chunk = makeChunk("prIV", anc);
    Bytes big = png;
    big.insert(big.begin() + 33, chunk.begin(), chunk.end());

    Collector c;
    size_t beforeChunk = unit_alloc_current();
    LodePNGRowDecoder* d = makeCollector(c);
    size_t base = unit_alloc_current();
    unit_alloc_reset_peak();
    UNIT_CHECK(lodepng_row_decoder_feed(d, big.data(), big.size(), 1) == 0);
    peaks[variant] = unit_alloc_peak() - base;
    UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
    lodepng_row_decoder_destroy(d);
    UNIT_CHECK(unit_alloc_current() == beforeChunk);
  }
  UNIT_CHECK(peaks[0] == p1);
  UNIT_CHECK(peaks[1] == p1);
}

void case_rows_interleaved() {
  unsigned wa = 11, ha = 13;
  unsigned wb = 27, hb = 5;
  Bytes pxa = makePixels(wa, ha, 71);
  Bytes pxb = makePixels(wb, hb, 91);
  Bytes pngA = encodeRGBA(wa, ha, pxa, 2);
  Bytes pngB = encodeRGBA(wb, hb, pxb, 1);
  unsigned rwa, rha, rwb, rhb;
  Bytes expA = decodeOld32(pngA, rwa, rha);
  Bytes expB = decodeOld32(pngB, rwb, rhb);

  Collector ca, cb;
  LodePNGRowDecoder* da = makeCollector(ca);
  LodePNGRowDecoder* db = makeCollector(cb);
  size_t pa = 0, pb = 0;
  while(pa < pngA.size() || pb < pngB.size()) {
    if(pa < pngA.size()) {
      size_t n = pngA.size() - pa;
      if(n > 5) n = 5;
      UNIT_CHECK(lodepng_row_decoder_feed(da, pngA.data() + pa, n, 0) == 0);
      pa += n;
    }
    if(pb < pngB.size()) {
      size_t n = pngB.size() - pb;
      if(n > 7) n = 7;
      UNIT_CHECK(lodepng_row_decoder_feed(db, pngB.data() + pb, n, 0) == 0);
      pb += n;
    }
  }
  UNIT_CHECK(lodepng_row_decoder_feed(da, 0, 0, 1) == 0);
  UNIT_CHECK(lodepng_row_decoder_feed(db, 0, 0, 1) == 0);
  UNIT_CHECK(lodepng_row_decoder_done(da) == 1);
  UNIT_CHECK(lodepng_row_decoder_done(db) == 1);
  verifyCollector(ca, wa, ha, expA);
  verifyCollector(cb, wb, hb, expB);
  lodepng_row_decoder_destroy(da);
  lodepng_row_decoder_destroy(db);
}

/* ------------------------- framing group ------------------------- */

void case_framing_null_and_empty() {
  UNIT_CHECK(lodepng_row_decoder_create(0, 0) == 0);
  UNIT_CHECK(lodepng_row_decoder_done(0) == 0);
  lodepng_row_decoder_destroy(0);

  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  UNIT_CHECK(d != 0);
  UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == 0);
  UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 1) != 0);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);
}

void case_framing_every_final_prefix_fails() {
  unsigned w = 5, h = 3;
  Bytes px = makePixels(w, h, 5);
  Bytes png = encodeRGBA(w, h, px, 1);
  for(size_t len = 0; len < png.size(); ++len) {
    Collector c;
    LodePNGRowDecoder* d = makeCollector(c);
    unsigned error = lodepng_row_decoder_feed(d, png.data(), len, 1);
    UNIT_CHECK(error != 0);
    UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
    UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == error);
    lodepng_row_decoder_destroy(d);
  }
}

void case_framing_bad_crc() {
  unsigned w = 9, h = 4;
  Bytes px = makePixels(w, h, 6);
  Bytes png = encodeRGBA(w, h, px, 2);
  std::vector<ChunkInfo> chunks = findChunks(png);
  size_t crcOffset = 0;
  for(size_t i = 0; i < chunks.size(); ++i) {
    if(std::memcmp(chunks[i].type, "IDAT", 4) == 0) {
      crcOffset = chunks[i].pos + 8 + chunks[i].len;
    }
  }
  UNIT_CHECK(crcOffset != 0 && crcOffset + 4 <= png.size());
  png[crcOffset] ^= 0xff;

  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  unsigned error = lodepng_row_decoder_feed(d, png.data(), png.size(), 1);
  UNIT_CHECK(error == 57);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  UNIT_CHECK(c.calls == h);
  unsigned calls = c.calls;
  UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == 57);
  UNIT_CHECK(c.calls == calls);
  lodepng_row_decoder_destroy(d);

  Bytes anc(64, 0x33);
  Bytes chunk = makeChunk("prIV", anc);
  chunk[chunk.size() - 1] ^= 1;
  Bytes bad = encodeRGBA(w, h, px, 0);
  bad.insert(bad.begin() + 33, chunk.begin(), chunk.end());
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, bad.data(), bad.size(), 1) == 57);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);
}

void case_framing_bad_adler() {
  unsigned w = 12, h = 6;
  Bytes px = makePixels(w, h, 8);
  Bytes png = encodeRGBA(w, h, px, 2);
  std::vector<ChunkInfo> chunks = findChunks(png);
  size_t adlerOffset = 0;
  size_t idatPos = 0, idatLen = 0;
  for(size_t i = chunks.size(); i-- > 0;) {
    if(std::memcmp(chunks[i].type, "IDAT", 4) == 0) {
      idatPos = chunks[i].pos;
      idatLen = chunks[i].len;
      adlerOffset = idatPos + 8 + idatLen - 1;
      break;
    }
  }
  UNIT_CHECK(adlerOffset != 0);
  png[adlerOffset] ^= 0x01;
  /* Repair the chunk CRC so error 58 comes from the Adler content path. */
  unsigned crc = lodepng_crc32(png.data() + idatPos + 4, idatLen + 4);
  put32(png, idatPos + 8 + idatLen, crc);

  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  unsigned error = lodepng_row_decoder_feed(d, png.data(), png.size(), 1);
  UNIT_CHECK(error == 58);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  UNIT_CHECK(c.calls == h);
  UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == 58);
  lodepng_row_decoder_destroy(d);
}

void mutateIHDR(Bytes& png, unsigned byteIndex, unsigned char value) {
  png[16 + byteIndex] = value;
  unsigned crc = lodepng_crc32(png.data() + 12, 17);
  put32(png, 29, crc);
}

void case_framing_extra_and_unsupported() {
  unsigned w = 6, h = 4;
  Bytes px = makePixels(w, h, 10);
  Bytes base = encodeRGBA(w, h, px, 0);

  Bytes extra = base;
  put32(extra, 20, 2); /* claim two rows while stream has four */
  unsigned crc = lodepng_crc32(extra.data() + 12, 17);
  put32(extra, 29, crc);
  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  unsigned error = lodepng_row_decoder_feed(d, extra.data(), extra.size(), 1);
  UNIT_CHECK(error == 91);
  UNIT_CHECK(c.calls == 2);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);

  Bytes grey = base;
  mutateIHDR(grey, 9, 0);
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, grey.data(), grey.size(), 1) == 31);
  lodepng_row_decoder_destroy(d);

  Bytes sixteen = base;
  mutateIHDR(sixteen, 8, 16);
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, sixteen.data(), sixteen.size(), 1) == 31);
  lodepng_row_decoder_destroy(d);

  Bytes adam7 = base;
  mutateIHDR(adam7, 12, 1);
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, adam7.data(), adam7.size(), 1) == 34);
  lodepng_row_decoder_destroy(d);
}

void case_framing_unknown_critical_plte() {
  unsigned w = 4, h = 2;
  Bytes px = makePixels(w, h, 2);
  Bytes png = encodeRGBA(w, h, px, 0);

  Bytes unknownData(8, 0xab);
  Bytes unknown = makeChunk("ZKrt", unknownData);
  Bytes bad = png;
  bad.insert(bad.begin() + 33, unknown.begin(), unknown.end());
  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, bad.data(), bad.size(), 1) == 69);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);

  Bytes plte(3, 0);
  Bytes plteChunk = makeChunk("PLTE", plte);
  Bytes goodPLTE = png;
  goodPLTE.insert(goodPLTE.begin() + 33, plteChunk.begin(), plteChunk.end());
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, goodPLTE.data(), goodPLTE.size(), 1) == 0);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
  lodepng_row_decoder_destroy(d);

  Bytes badPLTE(4, 0);
  Bytes badChunk = makeChunk("PLTE", badPLTE);
  Bytes badPLTEPng = png;
  badPLTEPng.insert(badPLTEPng.begin() + 33, badChunk.begin(), badChunk.end());
  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, badPLTEPng.data(), badPLTEPng.size(), 1) == 38);
  lodepng_row_decoder_destroy(d);
}

void case_framing_trailing_and_missing() {
  unsigned w = 7, h = 3;
  Bytes px = makePixels(w, h, 77);
  Bytes png = encodeRGBA(w, h, px, 1);

  Bytes trailing = png;
  trailing.push_back(0x99);
  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, trailing.data(), trailing.size(), 1) == 30);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);

  d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, png.data(), png.size(), 1) == 0);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 1);
  UNIT_CHECK(lodepng_row_decoder_feed(d, 0, 0, 0) == 0);
  unsigned char byte = 1;
  UNIT_CHECK(lodepng_row_decoder_feed(d, &byte, 1, 0) == 30);
  lodepng_row_decoder_destroy(d);

  static const size_t missing[3] = {1, 4, 12};
  for(int k = 0; k < 3; ++k) {
    size_t len = png.size() - missing[k];
    d = makeCollector(c);
    UNIT_CHECK(lodepng_row_decoder_feed(d, png.data(), len, 1) != 0);
    UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
    lodepng_row_decoder_destroy(d);
  }
}

void case_framing_invalid_filter() {
  Bytes raw;
  raw.push_back(9);
  raw.push_back(1);
  raw.push_back(2);
  raw.push_back(3);
  raw.push_back(4);
  Bytes def = storedDeflate(Bytes(), raw);
  Bytes z = zlibWrap(def, raw);
  Bytes png = makePNGFromZlib(1, 1, z);
  Collector c;
  LodePNGRowDecoder* d = makeCollector(c);
  UNIT_CHECK(lodepng_row_decoder_feed(d, png.data(), png.size(), 1) == 36);
  UNIT_CHECK(c.calls == 0);
  UNIT_CHECK(lodepng_row_decoder_done(d) == 0);
  lodepng_row_decoder_destroy(d);
}

} /* namespace */

void register_tests(std::vector<UFCase>& cases) {
  cases.push_back({"stored_two_blocks", "deflate", case_deflate_stored});
  cases.push_back({"fixed_overlap_window_wrap", "deflate", case_deflate_fixed_window});
  cases.push_back({"dynamic_code_length_repeats", "deflate", case_deflate_dynamic_repeats});
  cases.push_back({"encoded_three_block_types", "deflate", case_deflate_encoded_block_types});
  cases.push_back({"custom_zlib_and_inflate", "deflate", case_deflate_custom_settings});

  cases.push_back({"known_pixels_five_filters", "rows", case_rows_known_filters});
  cases.push_back({"each_filter_and_previous_row", "rows", case_rows_each_filter});
  cases.push_back({"all_single_cuts_filtered", "rows", case_rows_single_cuts});
  cases.push_back({"bytewise_and_reused_buffer", "rows", case_rows_bytewise_reuse});
  cases.push_back({"early_callback_before_idat_end", "rows", case_rows_early_callback});
  cases.push_back({"peak_growth_width_only", "rows", case_rows_memory_shape});
  cases.push_back({"interleaved_decoders", "rows", case_rows_interleaved});

  cases.push_back({"null_create_and_empty_final", "framing", case_framing_null_and_empty});
  cases.push_back({"every_final_prefix_fails", "framing", case_framing_every_final_prefix_fails});
  cases.push_back({"bad_chunk_crc_after_rows", "framing", case_framing_bad_crc});
  cases.push_back({"bad_adler_after_rows", "framing", case_framing_bad_adler});
  cases.push_back({"extra_scanline_and_unsupported", "framing", case_framing_extra_and_unsupported});
  cases.push_back({"unknown_critical_and_plte", "framing", case_framing_unknown_critical_plte});
  cases.push_back({"trailing_and_missing_iend", "framing", case_framing_trailing_and_missing});
  cases.push_back({"invalid_filter_content_error", "framing", case_framing_invalid_filter});
}
