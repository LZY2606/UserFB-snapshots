// Task test suite for the optional zero-motion P-picture (inter) encoder
// mode - see encoder/mpg_inter_encode.h and docs/encoding.md. Registered
// through task_support's task_cases/TASK_CHECK harness (no main() here -
// task_support/driver.c owns that), in three groups:
//
//   syntax    - independent bitstream inspection: picture_coding_type,
//               temporal_reference, sequence/GOP header placement, P
//               header forward parameters, and per-slice macroblock
//               address increments / macroblock_type / CBP (parsed with
//               the decoder's own bit-level read helpers straight out of
//               the emitted bytes, never encoder-internal state).
//   reference - the real TinyMPGDecoder decode path: all three planes of
//               I/P chains, chroma-only changes, per-frame QP changes, and
//               multi-generation reference behavior.
//   sequence  - the public state machine: I-then-P ordering,
//               resetSequence(), mode toggling, size changes (valid and
//               invalid), short-buffer/alloc failure retry with
//               byte-identical output, RGB/YUYV entry points, temporal
//               reference wraparound past 1024, and default-off
//               backward compatibility.
//
// Content is deliberately synthetic and deterministic (no asset
// dependencies): non-flat textures, explicit positive/negative residual
// deltas, chroma-only changes, and pictures built from a decoded
// reconstruction so "zero residual" macroblocks are exactly zero.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// task_support/driver.c is fed to the C++ compiler by run.sh, so check.h's
// declarations keep C++ linkage here (no extern "C" wrapper).
#include "task_support/check.h"

#include "TinyMPGDecoder.h"
#include "TinyMPGEncoder.h"
#include "decoder/mpg_headers.h"
#include "decoder/mpg_macroblock.h"  // decodeMbAddrIncrement/decodeMbTypeP/decodeCodedBlockPattern/...
#include "decoder/mpg_motion.h"

using namespace tinympg;

namespace {

const int kQp = 8;

size_t frameBytes(int w, int h) { return (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2); }
size_t ySize(int w, int h) { return (size_t)w * h; }
size_t cSize(int w, int h) { return (size_t)(w / 2) * (h / 2); }

// One start-code-delimited unit found in an elementary stream.
struct Unit {
  uint8_t type;
  const uint8_t* data;
  size_t size;
};

std::vector<Unit> scanUnits(const uint8_t* buf, size_t n) {
  std::vector<Unit> out;
  for (size_t i = 0; i + 3 < n; i++) {
    if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 1) continue;
    size_t start = i + 4;
    size_t end = n;
    for (size_t j = start; j + 2 < n; j++) {
      if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
        end = j;
        break;
      }
    }
    out.push_back(Unit{buf[i + 3], buf + start, end - start});
    i = start - 1;
  }
  return out;
}

int countUnits(const std::vector<Unit>& units, uint8_t type) {
  int n = 0;
  for (size_t i = 0; i < units.size(); i++)
    if (units[i].type == type) n++;
  return n;
}

struct PicHeaderInfo {
  int temporalReference;
  int codingType;
  bool fullPelForward;
  int forwardFCode;
};

bool readPicHeader(const Unit& u, PicHeaderInfo* out) {
  BitReader br(u.data, u.size);
  PictureInfo pic;
  if (!parsePictureHeader(br, &pic)) return false;
  out->temporalReference = pic.temporalReference;
  out->codingType = pic.codingType;
  out->fullPelForward = pic.fullPelForwardVector;
  out->forwardFCode = pic.forwardFCode;
  return true;
}

std::vector<PicHeaderInfo> pictureHeaders(const std::vector<Unit>& units) {
  std::vector<PicHeaderInfo> out;
  for (size_t i = 0; i < units.size(); i++) {
    if (units[i].type != 0x00) continue;
    PicHeaderInfo info;
    if (!readPicHeader(units[i], &info)) {
      out.clear();
      return out;
    }
    out.push_back(info);
  }
  return out;
}

// Per-macroblock record from an independent P-slice parse.
struct MbRecord {
  int addressIncrement;  // raw macroblock_address_increment (1 = adjacent)
  int typeRow;           // decodeMbTypeP() table row (1 = zero MV + CBP, 2 = explicit MV, no CBP)
  int cbp;               // 6-bit CBP, or -1 when macroblock_type carries none
};

struct SliceRecord {
  int row;
  int qscale;
  std::vector<MbRecord> mbs;
};

// Parses one slice payload exactly the way Decoder::decodeSliceP() does,
// but only records syntax (and runs the coefficient decoders far enough to
// validate every block's run/level/EOB structure, not just its headers).
bool parseOnePSlice(const Unit& u, int fCode, SliceRecord* rec) {
  BitReader br(u.data, u.size);
  rec->qscale = (int)br.u(5);
  if (rec->qscale == 0) return false;
  while (br.flag()) br.skipBits(8);
  if (br.error()) return false;

  int32_t lastMv[2] = {0, 0};
  for (;;) {
    MbAddrIncrement inc = decodeMbAddrIncrement(br);
    if (inc.error) return false;
    if (inc.endOfSlice) break;

    MbRecord mb;
    mb.addressIncrement = inc.value + 1;  // decode returns increment-1
    MbTypeP mt = decodeMbTypeP(br);
    if (mt.error || mt.intra) return false;  // this encoder never puts intra MBs in a P picture
    mb.typeRow = -1;
    // Recover the table row: only rows 1 (zero MV + CBP) and 2 (MV, no CBP) are emitted.
    mb.cbp = -1;
    if (mt.zeroMv && mt.hasCbp) {
      mb.typeRow = 1;
    } else if (!mt.zeroMv && !mt.hasCbp) {
      mb.typeRow = 2;
      MotionResult rx = decodeMotionComponent(br, fCode, lastMv[0]);
      MotionResult ry = decodeMotionComponent(br, fCode, lastMv[1]);
      if (rx.error || ry.error || rx.value != 0 || ry.value != 0) return false;
    } else {
      return false;  // un-emitted macroblock_type variant
    }

    if (mt.hasCbp) {
      CbpResult cbp = decodeCodedBlockPattern(br);
      if (cbp.error) return false;
      mb.cbp = cbp.pattern;
      int32_t coef[64];
      for (int b = 0; b < 6; b++) {
        if (!(cbp.pattern & (1 << (5 - b)))) continue;
        if (!decodeInterBlock(br, kDefaultNonIntraMatrix, rec->qscale, coef)) return false;
      }
    }
    if (br.error()) return false;
    rec->mbs.push_back(mb);
  }
  return true;
}

std::vector<SliceRecord> parsePSlices(const std::vector<Unit>& units, int mbHeight, int fCode) {
  std::vector<SliceRecord> out;
  for (int row = 0; row < mbHeight; row++) {
    const Unit* slice = nullptr;
    for (size_t i = 0; i < units.size(); i++) {
      if (units[i].type == (uint8_t)(1 + row)) {
        slice = &units[i];
        break;
      }
    }
    if (!slice) {
      out.clear();
      return out;
    }
    SliceRecord rec;
    rec.row = row;
    if (!parseOnePSlice(*slice, fCode, &rec)) {
      out.clear();
      return out;
    }
    out.push_back(rec);
  }
  return out;
}

// --- synthetic content -----------------------------------------------------

void fillTexture(std::vector<uint8_t>& frame, int w, int h, int t) {
  uint8_t* y = frame.data();
  uint8_t* u = y + ySize(w, h);
  uint8_t* v = u + cSize(w, h);
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      int val = (xx * 5 + yy * 7 + t * 11 + ((xx ^ yy) & 31)) & 0xFF;
      y[yy * w + xx] = (uint8_t)val;
    }
  }
  for (int yy = 0; yy < h / 2; yy++) {
    for (int xx = 0; xx < w / 2; xx++) {
      u[yy * (w / 2) + xx] = (uint8_t)((90 + xx * 3 + t * 9) & 0xFF);
      v[yy * (w / 2) + xx] = (uint8_t)((140 + yy * 3 - t * 7) & 0xFF);
    }
  }
}

// Low-motion variant: the same non-flat texture with only a small,
// bounded per-picture level shift between pictures - the regime the
// zero-motion (no search) P mode is actually meant for. Fast-changing
// content would force every block to residual every time anyway.
void fillGentleMotion(std::vector<uint8_t>& frame, int w, int h, int t) {
  uint8_t* y = frame.data();
  uint8_t* u = y + ySize(w, h);
  uint8_t* v = u + cSize(w, h);
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      int val = xx * 5 + yy * 7 + ((xx ^ yy) & 31) + (t * 3 - 7);
      if (val < 0) val = 0;
      if (val > 255) val = 255;
      y[yy * w + xx] = (uint8_t)val;
    }
  }
  for (int yy = 0; yy < h / 2; yy++) {
    for (int xx = 0; xx < w / 2; xx++) {
      u[yy * (w / 2) + xx] = (uint8_t)(90 + xx * 3);
      v[yy * (w / 2) + xx] = (uint8_t)(140 + yy * 3);
    }
  }
}

// Decodes one self-contained (or chained) buffer and returns every decoded
// picture as packed I420, in display order.
struct Collector {
  std::vector<uint8_t> planes;
  int frames = 0;
  size_t planeBytes = 0;
};

void collectorCallback(TinyMPGDecoder<>& dec, void* userData) {
  Collector* c = (Collector*)userData;
  size_t need = frameBytes(dec.width(), dec.height());  // recompute per frame: sizes may differ
  size_t old = c->planes.size();
  c->planes.resize(old + need);
  size_t written = dec.toYUV420(c->planes.data() + old, need);
  if (written != need) {
    c->planes.clear();
    return;
  }
  c->planeBytes = need;
  c->frames++;
}

bool decodeAll(const uint8_t* bitstream, size_t n, Collector* out) {
  TinyMPGDecoder<> dec;
  out->planes.clear();
  out->frames = 0;
  out->planeBytes = 0;
  dec.setCallback(collectorCallback, out);
  auto status = dec.write(bitstream, n);
  return !dec.hasError() && status != TinyMPGDecoder<>::Status::kError &&
         status != TinyMPGDecoder<>::Status::kUnsupported;
}

double planePsnr(const uint8_t* a, const uint8_t* b, size_t n, int* maxDiffOut) {
  double sse = 0;
  int maxDiff = 0;
  for (size_t i = 0; i < n; i++) {
    int d = (int)a[i] - (int)b[i];
    if (d < 0) d = -d;
    if (d > maxDiff) maxDiff = d;
    sse += (double)d * (double)d;
  }
  if (maxDiffOut) *maxDiffOut = maxDiff;
  if (sse == 0) return 999.0;
  return 10.0 * std::log10(255.0 * 255.0 / (sse / (double)n));
}

// Encodes `count` textured frames through one inter-mode encoder,
// appending them onto `out` (does not clear it - callers building a
// multi-sequence stream across several calls, e.g. across
// resetSequence(), rely on the accumulation).
bool encodeChain(int w, int h, int count, std::vector<uint8_t>* out,
                  TinyMPGEncoder<>& enc) {
  std::vector<uint8_t> frame(frameBytes(w, h));
  std::vector<uint8_t> dst(1u << 18);
  for (int t = 0; t < count; t++) {
    fillTexture(frame, w, h, t);
    size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                frame.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    if (n == 0) return false;
    out->insert(out->end(), dst.data(), dst.data() + n);
  }
  return true;
}

// Allocator whose allocate() fails on an ordered call count - the same
// no-throw contract test_alloc_failure.cpp uses on the decode side.
template <typename T>
struct FailingAllocator {
  using value_type = T;
  static int callCount;
  static int failAtCall;
  FailingAllocator() noexcept = default;
  template <typename U>
  FailingAllocator(const FailingAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) noexcept {
    callCount++;
    if (failAtCall != 0 && callCount == failAtCall) return nullptr;
    return static_cast<T*>(malloc(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t) noexcept { free(p); }
  template <typename U>
  bool operator==(const FailingAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const FailingAllocator<U>&) const noexcept {
    return false;
  }
};
template <typename T>
int FailingAllocator<T>::callCount = 0;
template <typename T>
int FailingAllocator<T>::failAtCall = 0;

}  // namespace

// ===========================================================================
// syntax group
// ===========================================================================

// I then P picture types, temporal_reference counting, and sequence/GOP
// headers emitted only before the sequence's first I-picture.
static int syntax_ip_types_and_headers() {
  const int w = 48, h = 32, count = 5;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w, h, count, &stream, enc));

  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  TASK_CHECK(countUnits(units, 0xB3) == 1);  // exactly one sequence_header
  TASK_CHECK(countUnits(units, 0xB8) == 1);  // exactly one GOP header
  TASK_CHECK(countUnits(units, 0x00) == count);

  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == (size_t)count);
  for (int t = 0; t < count; t++) {
    TASK_CHECK(pics[t].temporalReference == t);
    TASK_CHECK(pics[t].codingType == (t == 0 ? 1 : 2));
  }
  return 0;
}

// P picture header carries legal zero-motion forward parameters.
static int syntax_p_header_forward_params() {
  const int w = 32, h = 32;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w, h, 2, &stream, enc));
  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == 2);
  TASK_CHECK(pics[0].codingType == 1);
  TASK_CHECK(pics[1].codingType == 2);
  TASK_CHECK(pics[1].fullPelForward == false);  // half-pel units
  TASK_CHECK(pics[1].forwardFCode == 1);        // smallest legal f_code (0 is forbidden)
  return 0;
}

// CBP, skipped macroblocks, empty blocks between coded ones, chroma-only
// changes, row-edge explicit coding, and legal zero-residual slice ends.
static int syntax_cbp_skips_and_slices() {
  const int w = 64, h = 32;  // 4 x 2 macroblocks
  const size_t fb = frameBytes(w, h);
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> base(fb), dst(1u << 16);
  fillTexture(base, w, h, 0);
  size_t nI = enc.encodeFrame(base.data(), base.data() + ySize(w, h),
                               base.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(nI > 0);
  std::vector<uint8_t> iBytes(dst.data(), dst.data() + nI);  // preserve before dst is reused

  // The P frame's source starts from the I frame's exact reconstruction,
  // so untouched macroblocks have a literally zero residual.
  Collector recon;
  TASK_CHECK(decodeAll(iBytes.data(), iBytes.size(), &recon));
  TASK_CHECK(recon.frames == 1);
  std::vector<uint8_t> next = recon.planes;

  // Strong luma change in interior MB (1,0): all four luma blocks, no chroma.
  for (int yy = 0; yy < 16; yy++) {
    for (int xx = 0; xx < 16; xx++) {
      size_t idx = (size_t)(16 + yy) * w + (16 + xx);
      int v = (int)next[idx] + ( ((xx + yy) & 1) ? 45 : -45 );
      next[idx] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
  }
  // Strong chroma-only change in interior MB (2,1): U and V 8x8 blocks.
  uint8_t* uPlane = next.data() + ySize(w, h);
  uint8_t* vPlane = uPlane + cSize(w, h);
  for (int yy = 0; yy < 8; yy++) {
    for (int xx = 0; xx < 8; xx++) {
      uPlane[(size_t)(8 + yy) * (w / 2) + (16 + xx)] = (uint8_t)(200 + ((xx + yy) & 15));
      vPlane[(size_t)(8 + yy) * (w / 2) + (16 + xx)] = (uint8_t)(40 + ((xx * 3) & 15));
    }
  }

  size_t nP = enc.encodeFrame(next.data(), next.data() + ySize(w, h),
                               next.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(nP > 0);
  std::vector<uint8_t> both = iBytes;  // dst was reused for the P picture; use the saved I
  both.insert(both.end(), dst.data(), dst.data() + nP);

  std::vector<Unit> units = scanUnits(dst.data(), nP);  // P picture alone
  std::vector<SliceRecord> slices = parsePSlices(units, h / 16, 1);
  TASK_CHECK(slices.size() == 2);

  // Every slice's first and last macroblock column is explicit.
  int totalSkips = 0, cbpMb = 0, motionOnlyMb = 0;
  for (size_t s = 0; s < slices.size(); s++) {
    TASK_CHECK(!slices[s].mbs.empty());
    TASK_CHECK(slices[s].qscale == kQp);
    TASK_CHECK(slices[s].mbs.size() >= 2);  // at least the two row edges, both explicit
    int covered = 0;
    for (size_t i = 0; i < slices[s].mbs.size(); i++) {
      covered += slices[s].mbs[i].addressIncrement;
      if (slices[s].mbs[i].addressIncrement > 1) totalSkips++;
    }
    TASK_CHECK(covered == w / 16);  // increments account for the whole row
  }
  TASK_CHECK(totalSkips >= 1);  // empty blocks between coded ones really use gaps

  int lumaCbp = -1, chromaCbp = -1;
  for (size_t s = 0; s < slices.size(); s++) {
    for (size_t i = 0; i < slices[s].mbs.size(); i++) {
      const MbRecord& mb = slices[s].mbs[i];
      if (mb.cbp >= 0) {
        cbpMb++;
        TASK_CHECK(mb.typeRow == 1);  // zero MV + CBP
        if ((mb.cbp & 0x3C) != 0 && (mb.cbp & 0x03) == 0) lumaCbp = mb.cbp;
        if ((mb.cbp & 0x03) != 0 && (mb.cbp & 0x3C) == 0) chromaCbp = mb.cbp;
      } else {
        motionOnlyMb++;
        TASK_CHECK(mb.typeRow == 2);  // explicit (0,0) MV, no residual
      }
    }
  }
  TASK_CHECK(cbpMb == 2);
  TASK_CHECK(motionOnlyMb >= 4);  // both rows' first/last zero-residual edge MBs, plus gaps' edges
  TASK_CHECK(lumaCbp == 0x3C);    // all four luma blocks, no chroma
  TASK_CHECK(chromaCbp == 0x03);  // both chroma blocks, no luma

  // And the whole thing still decodes through the real decode path.
  Collector got;
  TASK_CHECK(decodeAll(both.data(), both.size(), &got));
  TASK_CHECK(got.frames == 2);
  return 0;
}

// An all-zero-residual P picture is legal end to end: edge macroblocks use
// the explicit motion-only type and middle ones are skipped.
static int syntax_all_zero_residual_p() {
  const int w = 64, h = 32;
  const size_t fb = frameBytes(w, h);
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> base(fb), dst(1u << 16);
  fillTexture(base, w, h, 3);
  size_t nI = enc.encodeFrame(base.data(), base.data() + ySize(w, h),
                               base.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(nI > 0);
  Collector recon;
  TASK_CHECK(decodeAll(dst.data(), nI, &recon));
  TASK_CHECK(recon.frames == 1);

  size_t nP = enc.encodeFrame(recon.planes.data(), recon.planes.data() + ySize(w, h),
                               recon.planes.data() + ySize(w, h) + cSize(w, h), dst.data(),
                               dst.size());
  TASK_CHECK(nP > 0);
  std::vector<Unit> units = scanUnits(dst.data(), nP);
  std::vector<SliceRecord> slices = parsePSlices(units, h / 16, 1);
  TASK_CHECK(slices.size() == 2);
  for (size_t s = 0; s < slices.size(); s++) {
    for (size_t i = 0; i < slices[s].mbs.size(); i++) {
      TASK_CHECK(slices[s].mbs[i].cbp == -1);  // no residual anywhere
      TASK_CHECK(slices[s].mbs[i].typeRow == 2);
    }
  }
  return 0;
}

// ===========================================================================
// reference group
// ===========================================================================

// Full I/P chain through the real decoder, all three planes checked against
// the sources. The content is the low-motion texture (this mode has no
// motion search - its fidelity contract is for the gentle-motion regime).
static int reference_three_plane_roundtrip() {
  const int w = 48, h = 32, count = 6;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> frame(frameBytes(w, h)), dst(1u << 18), stream;
  for (int t = 0; t < count; t++) {
    fillGentleMotion(frame, w, h, t);
    size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                frame.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    TASK_CHECK(n > 0);
    stream.insert(stream.end(), dst.data(), dst.data() + n);
  }

  Collector got;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &got));
  TASK_CHECK(got.frames == count);

  for (int t = 0; t < count; t++) {
    fillGentleMotion(frame, w, h, t);
    const uint8_t* decPlane = got.planes.data() + (size_t)t * got.planeBytes;
    int maxY = 0, maxC = 0;
    double psnrY = planePsnr(frame.data(), decPlane, ySize(w, h), &maxY);
    double psnrC = planePsnr(frame.data() + ySize(w, h), decPlane + ySize(w, h),
                              2 * cSize(w, h), &maxC);
    TASK_CHECK(psnrY >= 36.0);
    TASK_CHECK(psnrC >= 36.0);
    TASK_CHECK(maxY <= 30);
    TASK_CHECK(maxC <= 30);
  }
  return 0;
}

// A zero-residual P reconstructs byte-identically to its reference, and a
// chain of them stays identical across multiple generations.
static int reference_multigeneration_zero_residual() {
  const int w = 32, h = 32, generations = 5;
  const size_t fb = frameBytes(w, h);
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> src(fb), dst(1u << 16);
  fillTexture(src, w, h, 1);
  size_t n = enc.encodeFrame(src.data(), src.data() + ySize(w, h),
                              src.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(n > 0);
  std::vector<uint8_t> iBytes(dst.data(), dst.data() + n);  // preserve before dst is reused
  Collector first;
  TASK_CHECK(decodeAll(iBytes.data(), iBytes.size(), &first));
  TASK_CHECK(first.frames == 1);
  std::vector<uint8_t> previous = first.planes;

  std::vector<uint8_t> stream = iBytes;
  for (int g = 0; g < generations; g++) {
    // Each source is exactly the previous reconstructed picture.
    n = enc.encodeFrame(previous.data(), previous.data() + ySize(w, h),
                         previous.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    TASK_CHECK(n > 0);
    stream.insert(stream.end(), dst.data(), dst.data() + n);
  }

  Collector all;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &all));
  TASK_CHECK(all.frames == generations + 1);
  for (int t = 1; t <= generations; t++) {
    const uint8_t* a = all.planes.data();
    const uint8_t* b = all.planes.data() + (size_t)t * all.planeBytes;
    for (size_t i = 0; i < all.planeBytes; i++) TASK_CHECK(a[i] == b[i]);
  }
  return 0;
}

// Chroma-only changes leave luma untouched and show up on U and V only.
static int reference_chroma_only_change() {
  const int w = 32, h = 32;
  const size_t fb = frameBytes(w, h);
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> a(fb), b(fb), dst(1u << 16);
  fillTexture(a, w, h, 0);
  size_t nI = enc.encodeFrame(a.data(), a.data() + ySize(w, h),
                               a.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(nI > 0);
  std::vector<uint8_t> iBytes(dst.data(), dst.data() + nI);  // preserve before dst is reused
  Collector recon;
  TASK_CHECK(decodeAll(iBytes.data(), iBytes.size(), &recon));
  b = recon.planes;
  uint8_t* u = b.data() + ySize(w, h);
  uint8_t* v = u + cSize(w, h);
  for (size_t i = 0; i < cSize(w, h); i++) {
    u[i] = (uint8_t)((u[i] + 37) & 0xFF);
    v[i] = (uint8_t)((v[i] - 29) & 0xFF);
  }
  size_t nP = enc.encodeFrame(b.data(), b.data() + ySize(w, h),
                               b.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
  TASK_CHECK(nP > 0);
  std::vector<uint8_t> both = iBytes;
  both.insert(both.end(), dst.data(), dst.data() + nP);

  Collector got;
  TASK_CHECK(decodeAll(both.data(), both.size(), &got));
  TASK_CHECK(got.frames == 2);
  const uint8_t* f0 = got.planes.data();
  const uint8_t* f1 = got.planes.data() + got.planeBytes;
  for (size_t i = 0; i < ySize(w, h); i++) TASK_CHECK(f0[i] == f1[i]);  // luma identical
  int chromaChanged = 0;
  for (size_t i = ySize(w, h); i < got.planeBytes; i++)
    if (f0[i] != f1[i]) chromaChanged++;
  TASK_CHECK(chromaChanged > 0);
  return 0;
}

// setQp may change per picture without disturbing the sequence; the slice
// qscale follows each call and every frame still decodes.
static int reference_per_frame_qp() {
  const int w = 32, h = 32, count = 4;
  const int qps[4] = {4, 12, 20, 8};
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(qps[0]));
  enc.setInterMode(true);

  std::vector<uint8_t> frame(frameBytes(w, h)), dst(1u << 16), stream;
  std::vector<size_t> frameStarts;
  for (int t = 0; t < count; t++) {
    TASK_CHECK(enc.setQp(qps[t]));
    fillTexture(frame, w, h, t);
    frameStarts.push_back(stream.size());
    size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                frame.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    TASK_CHECK(n > 0);
    stream.insert(stream.end(), dst.data(), dst.data() + n);
  }

  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == (size_t)count);
  TASK_CHECK(pics[0].codingType == 1);
  for (int t = 1; t < count; t++) TASK_CHECK(pics[t].codingType == 2);  // QP change doesn't reset

  Collector got;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &got));
  TASK_CHECK(got.frames == count);
  for (int t = 0; t < count; t++) {
    const uint8_t* begin = stream.data() + frameStarts[t];
    size_t len = (t + 1 < count ? frameStarts[t + 1] : stream.size()) - frameStarts[t];
    std::vector<Unit> one = scanUnits(begin, len);
    const Unit* slice = nullptr;
    for (size_t i = 0; i < one.size(); i++)
      if (one[i].type == 1) slice = &one[i];
    TASK_CHECK(slice != nullptr);
    BitReader br(slice->data, slice->size);
    TASK_CHECK((int)br.u(5) == qps[t]);
  }
  return 0;
}

// ===========================================================================
// sequence group
// ===========================================================================

static int sequence_first_i_then_p() {
  const int w = 16, h = 16;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  TASK_CHECK(enc.interMode() == false);  // default off
  enc.setInterMode(true);

  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w, h, 3, &stream, enc));
  std::vector<PicHeaderInfo> pics = pictureHeaders(scanUnits(stream.data(), stream.size()));
  TASK_CHECK(pics.size() == 3);
  TASK_CHECK(pics[0].codingType == 1);
  TASK_CHECK(pics[1].codingType == 2 && pics[2].codingType == 2);
  TASK_CHECK(pics[0].temporalReference == 0 && pics[1].temporalReference == 1 &&
             pics[2].temporalReference == 2);
  return 0;
}

static int sequence_reset_restarts_at_i() {
  const int w = 16, h = 16;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w, h, 3, &stream, enc));  // I, P, P
  enc.resetSequence();
  TASK_CHECK(encodeChain(w, h, 2, &stream, enc));  // I, P again

  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  TASK_CHECK(countUnits(units, 0xB3) == 2);  // headers resent by the fresh sequence
  TASK_CHECK(countUnits(units, 0xB8) == 2);
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == 5);
  TASK_CHECK(pics[0].codingType == 1 && pics[1].codingType == 2 && pics[2].codingType == 2);
  TASK_CHECK(pics[3].codingType == 1 && pics[3].temporalReference == 0);
  TASK_CHECK(pics[4].codingType == 2 && pics[4].temporalReference == 1);
  return 0;
}

static int sequence_mode_toggle_restarts() {
  const int w = 16, h = 16;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));

  enc.setInterMode(true);
  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w, h, 2, &stream, enc));  // I, P

  enc.setInterMode(false);
  TASK_CHECK(encodeChain(w, h, 2, &stream, enc));  // two self-contained I pictures

  enc.setInterMode(true);
  TASK_CHECK(encodeChain(w, h, 2, &stream, enc));  // restarts: I, P

  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == 6);
  TASK_CHECK(pics[0].codingType == 1 && pics[1].codingType == 2);
  TASK_CHECK(pics[2].codingType == 1 && pics[3].codingType == 1);
  TASK_CHECK(pics[4].codingType == 1 && pics[5].codingType == 2);
  // While off, every picture carried its own sequence header.
  TASK_CHECK(countUnits(units, 0xB3) == 1 + 2 + 1);
  return 0;
}

static int sequence_size_change_and_invalid_size() {
  const int w1 = 16, h1 = 16, w2 = 32, h2 = 32;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w1, h1));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> stream;
  TASK_CHECK(encodeChain(w1, h1, 2, &stream, enc));  // I, P at 16x16

  // Invalid sizes are rejected and must not interrupt the sequence.
  TASK_CHECK(enc.setSize(17, 16) == false);
  TASK_CHECK(enc.setSize(0, 16) == false);
  TASK_CHECK(enc.setSize(w1, h1) == true);  // same size: accepted, no restart
  std::vector<uint8_t> frame(frameBytes(w1, h1)), dst(1u << 16);
  fillTexture(frame, w1, h1, 9);
  size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w1, h1),
                              frame.data() + ySize(w1, h1) + cSize(w1, h1), dst.data(), dst.size());
  TASK_CHECK(n > 0);
  stream.insert(stream.end(), dst.data(), dst.data() + n);
  std::vector<Unit> u3 = scanUnits(dst.data(), n);
  std::vector<PicHeaderInfo> p3 = pictureHeaders(u3);
  TASK_CHECK(p3.size() == 1 && p3[0].codingType == 2 && p3[0].temporalReference == 2);

  // A real size change restarts the sequence at I.
  TASK_CHECK(enc.setSize(w2, h2));
  std::vector<uint8_t> big(frameBytes(w2, h2));
  fillTexture(big, w2, h2, 2);
  n = enc.encodeFrame(big.data(), big.data() + ySize(w2, h2),
                       big.data() + ySize(w2, h2) + cSize(w2, h2), dst.data(), dst.size());
  TASK_CHECK(n > 0);
  stream.insert(stream.end(), dst.data(), dst.data() + n);
  std::vector<Unit> u4 = scanUnits(dst.data(), n);
  std::vector<PicHeaderInfo> p4 = pictureHeaders(u4);
  TASK_CHECK(p4.size() == 1 && p4[0].codingType == 1 && p4[0].temporalReference == 0);
  TASK_CHECK(countUnits(u4, 0xB3) == 1 && countUnits(u4, 0xB8) == 1);

  Collector got;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &got));
  TASK_CHECK(got.frames == 4);
  return 0;
}

// Undersized output: zero bytes, no frame number consumed, reference
// untouched; retrying with enough space matches a never-failed instance.
static int sequence_short_buffer_retry() {
  const int w = 32, h = 32, count = 5;
  TinyMPGEncoder<> good, flaky;
  TASK_CHECK(good.setSize(w, h) && flaky.setSize(w, h));
  TASK_CHECK(good.setQp(kQp) && flaky.setQp(kQp));
  good.setInterMode(true);
  flaky.setInterMode(true);

  std::vector<uint8_t> frame(frameBytes(w, h)), big(1u << 17);
  std::vector<uint8_t> refStream, flakyStream;
  for (int t = 0; t < count; t++) {
    fillTexture(frame, w, h, t);
    size_t ng = good.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                   frame.data() + ySize(w, h) + cSize(w, h), big.data(), big.size());
    TASK_CHECK(ng > 0);
    refStream.insert(refStream.end(), big.data(), big.data() + ng);

    // First attempt deliberately too short (the I frame and a P frame both get this treatment).
    uint8_t tiny[8];
    size_t failed = flaky.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                       frame.data() + ySize(w, h) + cSize(w, h), tiny, sizeof(tiny));
    TASK_CHECK(failed == 0);
    TASK_CHECK(flaky.lastStatus() == EncodeStatus::kBufferTooSmall);

    size_t nf = flaky.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                    frame.data() + ySize(w, h) + cSize(w, h), big.data(), big.size());
    TASK_CHECK(nf == ng);
    TASK_CHECK(memcmp(big.data(), refStream.data() + refStream.size() - ng, ng) == 0);
    flakyStream.insert(flakyStream.end(), big.data(), big.data() + nf);
  }
  TASK_CHECK(refStream == flakyStream);  // byte-identical to a never-failed instance

  Collector got;
  TASK_CHECK(decodeAll(flakyStream.data(), flakyStream.size(), &got));
  TASK_CHECK(got.frames == count);
  return 0;
}

// Allocation failure: surfaced as kAllocationError with zero bytes, no
// temporal_reference consumed and no committed-reference pollution; after
// recovery the output matches a healthy instance byte-for-byte. Frame
// buffers only allocate on first use and after a (sequence-restarting)
// size change, so those are the points the on-demand failures target.
static int sequence_allocation_failure_retry() {
  using FailAlloc = FailingAllocator<uint8_t>;
  TinyMPGEncoder<FailAlloc> flaky;
  TinyMPGEncoder<> good;
  TASK_CHECK(flaky.setSize(16, 16) && good.setSize(16, 16));
  TASK_CHECK(flaky.setQp(kQp) && good.setQp(kQp));
  flaky.setInterMode(true);
  good.setInterMode(true);

  std::vector<uint8_t> big(1u << 16), goodStream, flakyStream;

  // One encoded picture at (w,h,variant): the flaky encoder first fails an
  // allocation, then retries successfully; bytes must match the healthy one.
  struct Step {
    int w, h, t;
    int failCall;
  };
  const Step steps[] = {
      {16, 16, 0, 1},  // first inter-frame allocation: fail the Y plane
      {32, 32, 0, 2},  // size change reallocates: fail the U plane
      {32, 32, 1, 0},  // no resize: a normal P, no allocation (0 = don't fail)
      {16, 16, 2, 3},  // another size change: fail the V plane
  };

  int prevW = 16, prevH = 16;
  for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
    const Step& st = steps[s];
    if (st.w != prevW || st.h != prevH) {
      TASK_CHECK(flaky.setSize(st.w, st.h));
      TASK_CHECK(good.setSize(st.w, st.h));
      prevW = st.w;
      prevH = st.h;
    }
    std::vector<uint8_t> frame(frameBytes(st.w, st.h));
    fillTexture(frame, st.w, st.h, st.t);
    const uint8_t* yp = frame.data();
    const uint8_t* up = yp + ySize(st.w, st.h);
    const uint8_t* vp = up + cSize(st.w, st.h);

    if (st.failCall != 0) {
      FailAlloc::callCount = 0;
      FailAlloc::failAtCall = st.failCall;
      size_t bad = flaky.encodeFrame(yp, up, vp, big.data(), big.size());
      TASK_CHECK(bad == 0);
      TASK_CHECK(flaky.lastStatus() == EncodeStatus::kAllocationError);
      FailAlloc::failAtCall = 0;  // allocations succeed again
    }

    size_t ng = good.encodeFrame(yp, up, vp, big.data(), big.size());
    TASK_CHECK(ng > 0);
    goodStream.insert(goodStream.end(), big.data(), big.data() + ng);
    size_t nf = flaky.encodeFrame(yp, up, vp, big.data(), big.size());
    TASK_CHECK(nf == ng);
    TASK_CHECK(memcmp(big.data(), goodStream.data() + goodStream.size() - ng, ng) == 0);
    flakyStream.insert(flakyStream.end(), big.data(), big.data() + nf);
  }
  TASK_CHECK(goodStream == flakyStream);

  // The failed calls consumed no frame numbers: every successful picture's
  // temporal_reference matches the healthy encoder's, and the stream still
  // decodes end to end (references were never polluted).
  std::vector<PicHeaderInfo> gp = pictureHeaders(scanUnits(goodStream.data(), goodStream.size()));
  std::vector<PicHeaderInfo> fp = pictureHeaders(scanUnits(flakyStream.data(), flakyStream.size()));
  TASK_CHECK(gp.size() == fp.size());
  for (size_t i = 0; i < gp.size(); i++) {
    TASK_CHECK(fp[i].codingType == gp[i].codingType);
    TASK_CHECK(fp[i].temporalReference == gp[i].temporalReference);
  }
  Collector got;
  TASK_CHECK(decodeAll(flakyStream.data(), flakyStream.size(), &got));
  TASK_CHECK(got.frames == (int)sizeof(steps) / sizeof(steps[0]));
  return 0;
}

// RGB888 and YUYV entry points share the one sequence state.
static int sequence_packed_entry_points() {
  const int w = 32, h = 32;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  enc.setInterMode(true);

  std::vector<uint8_t> rgb((size_t)w * h * 3);
  std::vector<uint8_t> yuyv((size_t)w * h * 2);
  std::vector<uint8_t> dst(1u << 17), stream;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t* p = &rgb[((size_t)y * w + x) * 3];
      p[0] = (uint8_t)(x * 7);
      p[1] = (uint8_t)(y * 9);
      p[2] = (uint8_t)(128 + x - y);
    }
  }
  for (size_t i = 0; i < yuyv.size(); i++) yuyv[i] = rgb[i % rgb.size()];

  size_t n1 = enc.encodeFrameRgb888(rgb.data(), dst.data(), dst.size());
  TASK_CHECK(n1 > 0);
  stream.insert(stream.end(), dst.data(), dst.data() + n1);
  size_t n2 = enc.encodeFrameYuv422(yuyv.data(), dst.data(), dst.size());
  TASK_CHECK(n2 > 0);
  stream.insert(stream.end(), dst.data(), dst.data() + n2);
  size_t n3 = enc.encodeFrameRgb888(rgb.data(), dst.data(), dst.size());
  TASK_CHECK(n3 > 0);
  stream.insert(stream.end(), dst.data(), dst.data() + n3);

  std::vector<PicHeaderInfo> pics = pictureHeaders(scanUnits(stream.data(), stream.size()));
  TASK_CHECK(pics.size() == 3);
  TASK_CHECK(pics[0].codingType == 1);  // RGB entry opens the sequence with I
  TASK_CHECK(pics[1].codingType == 2);  // YUYV entry continues it with P
  TASK_CHECK(pics[2].codingType == 2);

  Collector got;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &got));
  TASK_CHECK(got.frames == 3);
  return 0;
}

// temporal_reference wraps modulo 1024; picture types and the single
// sequence header hold across more than 1024 successful pictures.
static int sequence_temporal_reference_wraparound() {
  const int w = 16, h = 16, count = 1030;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(20));  // high QP keeps these tiny 1-MB frames small/fast
  enc.setInterMode(true);

  std::vector<uint8_t> frame(frameBytes(w, h)), dst(1u << 16), stream;
  for (int t = 0; t < count; t++) {
    fillTexture(frame, w, h, t);
    size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                frame.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    TASK_CHECK(n > 0);
    stream.insert(stream.end(), dst.data(), dst.data() + n);
  }

  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  TASK_CHECK(countUnits(units, 0xB3) == 1);
  TASK_CHECK(countUnits(units, 0xB8) == 1);
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == (size_t)count);
  const int probes[5] = {0, 1, 1023, 1024, 1029};
  for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
    int t = probes[i];
    TASK_CHECK(pics[t].temporalReference == (t % 1024));
    TASK_CHECK(pics[t].codingType == (t == 0 ? 1 : 2));
  }

  Collector got;
  TASK_CHECK(decodeAll(stream.data(), stream.size(), &got));
  TASK_CHECK(got.frames == count);
  return 0;
}

// Default-off behavior is preserved: every call is a self-contained I
// picture with its own headers and temporal reference 0.
static int sequence_default_off_compatibility() {
  const int w = 32, h = 32;
  TinyMPGEncoder<> enc;
  TASK_CHECK(enc.setSize(w, h));
  TASK_CHECK(enc.setQp(kQp));
  TASK_CHECK(enc.interMode() == false);

  std::vector<uint8_t> frame(frameBytes(w, h)), dst(1u << 16), stream;
  std::vector<size_t> starts;
  for (int t = 0; t < 3; t++) {
    fillTexture(frame, w, h, t);
    starts.push_back(stream.size());
    size_t n = enc.encodeFrame(frame.data(), frame.data() + ySize(w, h),
                                frame.data() + ySize(w, h) + cSize(w, h), dst.data(), dst.size());
    TASK_CHECK(n > 0);
    stream.insert(stream.end(), dst.data(), dst.data() + n);
  }
  std::vector<Unit> units = scanUnits(stream.data(), stream.size());
  TASK_CHECK(countUnits(units, 0xB3) == 3);
  TASK_CHECK(countUnits(units, 0xB8) == 3);
  std::vector<PicHeaderInfo> pics = pictureHeaders(units);
  TASK_CHECK(pics.size() == 3);
  for (int t = 0; t < 3; t++) {
    TASK_CHECK(pics[t].codingType == 1);
    TASK_CHECK(pics[t].temporalReference == 0);
  }
  // Each individual output is independently decodable.
  for (int t = 0; t < 3; t++) {
    size_t len = (t + 1 < 3 ? starts[t + 1] : stream.size()) - starts[t];
    Collector one;
    TASK_CHECK(decodeAll(stream.data() + starts[t], len, &one));
    TASK_CHECK(one.frames == 1);
  }
  return 0;
}

const struct task_case task_cases[] = {
    {"syntax", "ip_types_and_headers", syntax_ip_types_and_headers},
    {"syntax", "p_header_forward_params", syntax_p_header_forward_params},
    {"syntax", "cbp_skips_and_slices", syntax_cbp_skips_and_slices},
    {"syntax", "all_zero_residual_p", syntax_all_zero_residual_p},
    {"reference", "three_plane_roundtrip", reference_three_plane_roundtrip},
    {"reference", "multigeneration_zero_residual", reference_multigeneration_zero_residual},
    {"reference", "chroma_only_change", reference_chroma_only_change},
    {"reference", "per_frame_qp", reference_per_frame_qp},
    {"sequence", "first_i_then_p", sequence_first_i_then_p},
    {"sequence", "reset_restarts_at_i", sequence_reset_restarts_at_i},
    {"sequence", "mode_toggle_restarts", sequence_mode_toggle_restarts},
    {"sequence", "size_change_and_invalid_size", sequence_size_change_and_invalid_size},
    {"sequence", "short_buffer_retry", sequence_short_buffer_retry},
    {"sequence", "allocation_failure_retry", sequence_allocation_failure_retry},
    {"sequence", "packed_entry_points", sequence_packed_entry_points},
    {"sequence", "temporal_reference_wraparound", sequence_temporal_reference_wraparound},
    {"sequence", "default_off_compatibility", sequence_default_off_compatibility},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
