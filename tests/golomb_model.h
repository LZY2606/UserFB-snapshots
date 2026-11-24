// golomb_model.h
//
// Independent, bit-exact model of the ALAC adaptive-Golomb relationships:
//
//   * the divisor is m = 2^k - 1 (not a power of two);
//   * a zero remainder shortens the code by one bit (the "truncated
//     remainder": the decoder peeks k bits but only commits to k-1 unless
//     the peeked value is >= 2);
//   * reaching nine leading ones switches to a fixed-width direct store;
//   * low mean arms zmode: an optional zero run (coded with the 16-bit
//     dyn_get/dyn_code variant) followed by a sign/magnitude mapping that
//     is shifted by one (n + zmode) for the first non-zero residual;
//   * mean adaptation, the n > 65535 clamp and the n >= 65535 run boundary
//     that clears zmode.
//
// This header never calls dyn_code/dyn_get/dyn_comp/dyn_decomp/jam or the
// production BitBufferRead/Write helpers.  It only uses fixed-width integer
// arithmetic and its own MSB-first reader/writer.  The numeric format
// constants below mirror codec/aglib.h (they are part of the stream format)
// but use distinct names because the production header #defines the old ones.

#ifndef GOLOMB_MODEL_H
#define GOLOMB_MODEL_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace gm {

// Format constants (mirror codec/aglib.h).
static const uint32_t kQbShift        = 9;
static const uint32_t kQb             = 1u << kQbShift;    // 512
static const uint32_t kPb0            = 40;
static const uint32_t kMb0            = 10;
static const uint32_t kKb0            = 14;
static const uint32_t kMMulShift      = 2;
static const uint32_t kMDenShift      = kQbShift - kMMulShift - 1; // 6
static const uint32_t kMOff           = 1u << (kMDenShift - 2);    // 16
static const int32_t  kBitOff         = 24;
static const uint32_t kMaxPrefix16    = 9;
static const uint32_t kMaxPrefix32    = 9;
static const uint32_t kMaxDatatype16  = 16;
static const uint32_t kNMaxMeanClamp  = 0xffff;

static const int32_t  STATUS_OK       = 0;
static const int32_t  STATUS_PARAM    = -50; // kALAC_ParamError

// Count leading zero bits in a 32-bit word; lead32(0) == 32, matching lead().
inline int lead32(uint32_t x) {
    int j = 0;
    uint32_t c = (uint32_t)1 << 31;
    for (j = 0; j < 32; ++j) {
        if ((c & x) != 0) break;
        c >>= 1;
    }
    return j;
}

// lg3a(x) == floor(log2(x + 3)) in the production sources.
inline int lg3a(int32_t x) {
    x += 3;
    int32_t result = lead32((uint32_t)x);
    return 31 - result;
}

// k used for a zero-run length code once the mean has gone low.
inline uint32_t runK(uint32_t mb) {
    int k = lead32(mb) - kBitOff + (int)((mb + kMOff) >> kMDenShift);
    return (uint32_t)k;
}

// ---------------------------------------------------------------------------
// MSB-first bit writer that accumulates 0/1 bits.
// ---------------------------------------------------------------------------
struct BitList {
    std::vector<int> b;
    void one(int x) { b.push_back(x ? 1 : 0); }
    void bits(uint32_t value, uint32_t n) {
        for (int32_t i = (int32_t)n - 1; i >= 0; --i)
            b.push_back((value >> i) & 1u);
    }
    size_t size() const { return b.size(); }
};

// Pack the accumulated bits into bytes starting at absolute bit position
// 'startBit' (0..7) inside byte 0.  Other bits keep the 'prefill' pattern.
inline std::vector<uint8_t> packAt(const std::vector<int>& bits,
                                   uint32_t startBit,
                                   size_t byteCount,
                                   uint8_t prefill) {
    std::vector<uint8_t> out(byteCount, prefill);
    for (size_t i = 0; i < bits.size(); ++i) {
        size_t absPos = (size_t)startBit + i;
        size_t byte = absPos >> 3;
        uint32_t bitInByte = 7u - (absPos & 7u);
        if (byte >= byteCount) break;
        if (bits[i]) out[byte] |=  (uint8_t)(1u << bitInByte);
        else         out[byte] &= (uint8_t)~(1u << bitInByte);
    }
    return out;
}

// Independent MSB-first byte reader with absolute bit positions.  Reads are
// bit-by-bit so behaviour is obvious; callers supply padded storage.
struct ByteReader {
    const uint8_t* p;
    size_t bytes;
    ByteReader(const uint8_t* p_, size_t bytes_) : p(p_), bytes(bytes_) {}

    int bitAt(size_t absPos) const {
        size_t byte = absPos >> 3;
        // Padded storage makes out-of-range reads deterministic zero.
        if (byte >= bytes) return 0;
        return (p[byte] >> (7 - (absPos & 7u))) & 1;
    }
    uint32_t readBits(size_t absPos, uint32_t n) const {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i)
            v = (v << 1) | (uint32_t)bitAt(absPos + i);
        return v;
    }
    // Number of consecutive one bits starting at absPos, capped at limit.
    uint32_t countOnes(size_t absPos, uint32_t limit) const {
        uint32_t n = 0;
        while (n < limit && bitAt(absPos + n) == 1) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------
// Code-word model.
// ---------------------------------------------------------------------------
struct CodeInfo {
    // inputs / state
    uint32_t n = 0;
    uint32_t k = 0;
    uint32_t m = 0;
    uint32_t divv = 0;
    uint32_t modd = 0;
    bool zeroRemainder = false;
    bool escape = false;
    // layout
    size_t bitStart = 0;   // relative to block start
    size_t bitLen = 0;
};

// Encode one main residual index n with the dyn_code_32bit rules.
inline CodeInfo encodeMainCode(BitList& out, uint32_t n, uint32_t m,
                               uint32_t k, uint32_t maxBits) {
    CodeInfo ci;
    ci.n = n; ci.k = k; ci.m = m; ci.bitStart = out.size();
    uint32_t divv = n / m;
    ci.divv = divv;

    bool escape = false;
    uint32_t numBits = 0;
    uint32_t modd = 0, de = 0;

    if (divv >= kMaxPrefix32) {
        escape = true;
    } else {
        modd = n - m * divv;
        de = (modd == 0) ? 1u : 0u;
        numBits = divv + k + 1 - de;
        if (numBits > 25) escape = true; // production "goto codeasescape"
    }

    ci.escape = escape;
    if (escape) {
        // Nine ones followed by the raw index in exactly maxBits bits.
        for (uint32_t i = 0; i < kMaxPrefix32; ++i) out.one(1);
        out.bits(n, maxBits);
        ci.bitLen = out.size() - ci.bitStart;
        return ci;
    }

    ci.modd = modd;
    ci.zeroRemainder = (de == 1);

    // div leading ones.
    for (uint32_t i = 0; i < divv; ++i) out.one(1);

    if (de) {
        // Zero remainder: terminator zero plus only k-1 remainder bits,
        // which together are k zero bits after the prefix.
        out.bits(0, k);
    } else {
        // Non-zero remainder: terminator zero then the k-bit value mod+1.
        out.one(0);
        out.bits(modd + 1, k);
    }
    ci.bitLen = out.size() - ci.bitStart;
    return ci;
}

// Decode one main code.  'pos' is relative to block start and is advanced.
inline CodeInfo decodeMainCode(const ByteReader& r, size_t& pos,
                               uint32_t m, uint32_t k, uint32_t maxBits) {
    CodeInfo ci;
    ci.k = k; ci.m = m; ci.bitStart = pos;

    uint32_t pre = r.countOnes(pos, kMaxPrefix32 + 1);
    if (pre >= kMaxPrefix32) {
        ci.escape = true;
        pos += kMaxPrefix32;
        ci.n = r.readBits(pos, maxBits);
        pos += maxBits;
        ci.bitLen = pos - ci.bitStart;
        return ci;
    }

    ci.divv = pre;
    pos += pre;      // leading ones
    pos += 1;        // zero terminator

    if (k == 1) {
        // m == 1: no remainder bits at all; result is the prefix count.
        ci.n = pre * m;
        ci.bitLen = pos - ci.bitStart;
        ci.zeroRemainder = true;
        return ci;
    }

    // Peek k bits; only k-1 are committed unless v >= 2.
    uint32_t v = r.readBits(pos, k);
    uint32_t result = pre * m;
    pos += k;
    pos -= 1;                 // truncated remainder commits k-1
    if (v >= 2) {
        result += (v - 1);
        pos += 1;
        ci.zeroRemainder = false;
    } else {
        ci.zeroRemainder = true;
    }
    ci.n = result;
    ci.modd = (m == 0) ? 0 : (result - pre * m);
    ci.bitLen = pos - ci.bitStart;
    return ci;
}

// Encode one zero-run length with the 16-bit dyn_code rules.
inline CodeInfo encodeRunCode(BitList& out, uint32_t nz, uint32_t mz,
                              uint32_t k) {
    CodeInfo ci;
    ci.n = nz; ci.k = k; ci.m = mz; ci.bitStart = out.size();
    uint32_t divv = (mz == 0) ? 0 : nz / mz;
    ci.divv = divv;

    if (divv >= kMaxPrefix16) {
        ci.escape = true;
        for (uint32_t i = 0; i < kMaxPrefix16; ++i) out.one(1);
        out.bits(nz, kMaxDatatype16);
        ci.bitLen = out.size() - ci.bitStart;
        return ci;
    }

    uint32_t modd = nz % mz;
    uint32_t de = (modd == 0) ? 1u : 0u;
    ci.modd = modd;
    ci.zeroRemainder = (de == 1);
    for (uint32_t i = 0; i < divv; ++i) out.one(1);
    if (de) {
        out.bits(0, k);
    } else {
        out.one(0);
        out.bits(modd + 1, k);
    }
    ci.bitLen = out.size() - ci.bitStart;
    return ci;
}

// Decode one zero-run code (dyn_get).
inline CodeInfo decodeRunCode(const ByteReader& r, size_t& pos,
                              uint32_t mz, uint32_t k) {
    CodeInfo ci;
    ci.k = k; ci.m = mz; ci.bitStart = pos;

    uint32_t pre = r.countOnes(pos, kMaxPrefix16 + 1);
    if (pre >= kMaxPrefix16) {
        ci.escape = true;
        pos += kMaxPrefix16;
        ci.n = r.readBits(pos, kMaxDatatype16);
        pos += kMaxDatatype16;
        ci.bitLen = pos - ci.bitStart;
        return ci;
    }

    ci.divv = pre;
    pos += pre;
    pos += 1;
    uint32_t v = r.readBits(pos, k);
    pos += k;
    uint32_t result = pre * mz + v - 1;
    if (v < 2) {
        result -= (v - 1); // v == 0 adds one back; v == 1 unchanged
        pos -= 1;
        ci.zeroRemainder = true;
    }
    ci.n = result;
    ci.bitLen = pos - ci.bitStart;
    return ci;
}

// Signed residual -> unsigned interleaved index used by the encoder.
// n = 2*|del| - (del < 0) - zmode.
inline uint32_t residualToN(int32_t del, uint32_t zmode) {
    uint32_t absd = (del < 0) ? (uint32_t)(-(int64_t)del) : (uint32_t)del;
    uint32_t signBit = (del < 0) ? 1u : 0u;
    return (absd << 1) - signBit - zmode;
}

// Unsigned index -> signed residual with the zmode shift correction.
inline int32_t nToResidual(uint32_t n, uint32_t zmode) {
    uint32_t ndecode = n + zmode;
    int32_t multiplier = (ndecode & 1u) ? -1 : 1;
    return (int32_t)(((ndecode + 1u) >> 1) * (uint32_t)multiplier);
}

// ---------------------------------------------------------------------------
// Whole-block model.
// ---------------------------------------------------------------------------
struct MainSym {
    CodeInfo code;
    int32_t del = 0;
    uint32_t meanBefore = 0;
    uint32_t meanAfter = 0;
    uint32_t zmodeBefore = 0;
};

struct RunSym {
    CodeInfo code;
    uint32_t length = 0;
    uint32_t meanAtRun = 0;
    uint32_t runK = 0;
    uint32_t runM = 0;
    bool clearsZmode = false; // n >= 65535
    size_t zeroStartIndex = 0;
};

struct Block {
    uint32_t mb0 = kMb0, pb = kPb0, kb = kKb0, wb = 0;
    uint32_t maxBits = 16;

    BitList encoded;                 // model bit stream, block-relative
    std::vector<MainSym> mains;
    std::vector<RunSym> runs;

    // decode results
    std::vector<int32_t> out;
    size_t consumed = 0;             // bits consumed minus start offset
    int32_t status = STATUS_OK;
    size_t samplesWrittenAtFail = 0; // for run-overrun diagnosis
};

inline uint32_t currentK(uint32_t mb, uint32_t kb) {
    uint32_t mraw = mb >> kQbShift;
    uint32_t k = (uint32_t)lg3a((int32_t)mraw);
    if (k > kb) k = kb;
    return k;
}

// Encoder-side model: explicit signed residual plan -> block bits.
inline Block encodePlan(const std::vector<int32_t>& dels,
                        uint32_t mb0 = kMb0, uint32_t pb = kPb0,
                        uint32_t kb = kKb0, uint32_t maxBits = 16) {
    Block blk;
    blk.mb0 = mb0; blk.pb = pb; blk.kb = kb;
    blk.wb = (kb >= 32) ? 0xffffffffu : ((1u << kb) - 1u);
    blk.maxBits = maxBits;

    uint32_t mb = mb0;
    uint32_t zmode = 0;
    size_t i = 0;
    const size_t N = dels.size();

    while (i < N) {
        uint32_t k = currentK(mb, kb);
        uint32_t m = (1u << k) - 1u;

        int32_t del = dels[i];
        uint32_t n = residualToN(del, zmode);

        MainSym ms;
        ms.del = del;
        ms.meanBefore = mb;
        ms.zmodeBefore = zmode;
        ms.code = encodeMainCode(blk.encoded, n, m, k, maxBits);

        mb = pb * (n + zmode) + mb - ((pb * mb) >> kQbShift);
        if (n > kNMaxMeanClamp) mb = kNMaxMeanClamp;
        ms.meanAfter = mb;
        blk.mains.push_back(ms);

        ++i;
        zmode = 0;

        if (((mb << kMMulShift) < kQb) && (i < N)) {
            zmode = 1;
            uint32_t nz = 0;
            size_t zeroStart = i;
            bool capped = false;
            while (i < N && dels[i] == 0) {
                ++i;
                ++nz;
                if (nz >= 65535) { zmode = 0; capped = true; break; }
            }

            uint32_t kr = runK(mb);
            uint32_t mz = ((1u << kr) - 1u) & blk.wb;

            RunSym rs;
            rs.length = nz;
            rs.meanAtRun = mb;
            rs.runK = kr;
            rs.runM = mz;
            rs.zeroStartIndex = zeroStart;
            rs.clearsZmode = capped;
            rs.code = encodeRunCode(blk.encoded, nz, mz, kr);
            blk.runs.push_back(rs);

            mb = 0;
        }
    }
    blk.consumed = blk.encoded.size();
    return blk;
}

// Decoder-side model over packed bytes.  'startBit' is the BitBuffer bitIndex
// (0..7) at entry with the reader base at byte 0.  'byteSizeBits' is the
// production maxPos convention (BitBuffer.byteSize * 8).
inline Block decodeBlock(const uint8_t* bytes, size_t byteCount,
                         uint32_t startBit, uint32_t numSamples,
                         uint32_t mb0, uint32_t pb, uint32_t kb,
                         uint32_t maxBits, size_t byteSizeBytes,
                         bool* eofHit = 0) {
    Block blk;
    blk.mb0 = mb0; blk.pb = pb; blk.kb = kb;
    blk.wb = (kb >= 32) ? 0xffffffffu : ((1u << kb) - 1u);
    blk.maxBits = maxBits;

    ByteReader r(bytes, byteCount);
    size_t pos = startBit;
    const size_t maxPos = (size_t)byteSizeBytes * 8u;
    uint32_t mb = mb0;
    uint32_t zmode = 0;
    uint32_t c = 0;
    if (eofHit) *eofHit = false;

    while (c < numSamples) {
        if (!(pos < maxPos)) { blk.status = STATUS_PARAM; if (eofHit) *eofHit = true; break; }

        uint32_t k = currentK(mb, kb);
        uint32_t m = (1u << k) - 1u;

        MainSym ms;
        ms.meanBefore = mb;
        ms.zmodeBefore = zmode;
        ms.code = decodeMainCode(r, pos, m, k, maxBits);
        uint32_t n = ms.code.n;

        int32_t del = nToResidual(n, zmode);
        ms.del = del;
        blk.out.push_back(del);
        ++c;

        mb = pb * (n + zmode) + mb - ((pb * mb) >> kQbShift);
        if (n > kNMaxMeanClamp) mb = kNMaxMeanClamp;
        ms.meanAfter = mb;
        blk.mains.push_back(ms);

        zmode = 0;

        if (((mb << kMMulShift) < kQb) && (c < numSamples)) {
            zmode = 1;
            uint32_t kr = runK(mb);
            uint32_t mz = ((1u << kr) - 1u) & blk.wb;

            RunSym rs;
            rs.meanAtRun = mb;
            rs.runK = kr;
            rs.runM = mz;
            rs.zeroStartIndex = c;
            rs.code = decodeRunCode(r, pos, mz, kr);
            uint32_t nrun = rs.code.n;
            rs.length = nrun;

            if (!(c + nrun <= numSamples)) {
                blk.status = STATUS_PARAM;
                blk.consumed = pos - startBit;
                blk.samplesWrittenAtFail = c;
                blk.runs.push_back(rs);
                return blk;
            }

            for (uint32_t j = 0; j < nrun; ++j) blk.out.push_back(0);
            c += nrun;

            if (nrun >= 65535) { zmode = 0; rs.clearsZmode = true; }
            blk.runs.push_back(rs);
            mb = 0;
        }
    }

    blk.consumed = pos - startBit;
    return blk;
}

// Convenience: decode a vector<int> bit stream.
inline Block decodeBitList(const std::vector<int>& bits, uint32_t startBit,
                           uint32_t numSamples, uint32_t mb0, uint32_t pb,
                           uint32_t kb, uint32_t maxBits) {
    // Pack block-relative bits at absolute startBit in a zero-padded image.
    size_t absBits = (size_t)startBit + bits.size() + 64;
    size_t byteCount = (absBits + 7) / 8;
    std::vector<uint8_t> img = packAt(bits, startBit, byteCount, 0x00);
    // byteSize convention: enough bytes to cover start + valid bits.
    size_t logicalBytes = ((size_t)startBit + bits.size() + 7) / 8;
    return decodeBlock(img.data(), img.size(), startBit, numSamples,
                       mb0, pb, kb, maxBits, logicalBytes);
}

} // namespace gm

#endif // GOLOMB_MODEL_H
