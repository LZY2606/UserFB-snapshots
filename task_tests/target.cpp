#include "../task_support/registry.hpp"

#include "bcdec.h"
#include "bcdec_transform.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Byte = unsigned char;
using BlockBytes = std::array<Byte, 16>;
using Pixels = std::array<Byte, 64>;
using Image8x8 = std::array<Byte, 256>;

const int OPS = 8;

int colorBits(int mode)
{
    static const int v[8] = { 4, 6, 5, 7, 5, 7, 7, 5 };
    return v[mode];
}
int alphaBits(int mode)
{
    static const int v[8] = { 0, 0, 0, 0, 6, 8, 7, 5 };
    return v[mode];
}
int indexBits1(int mode)
{
    static const int v[8] = { 3, 3, 2, 2, 2, 2, 4, 2 };
    return v[mode];
}
int indexBits2(int mode)
{
    static const int v[8] = { 0, 0, 0, 0, 3, 2, 0, 0 };
    return v[mode];
}
int subsetCount(int mode)
{
    static const int v[8] = { 3, 2, 3, 2, 1, 1, 1, 2 };
    return v[mode];
}
int partitionBits(int mode)
{
    if (mode == 0) return 4;
    if (mode == 1 || mode == 2 || mode == 3 || mode == 7) return 6;
    return 0;
}
int inverseOp(int op)
{
    static const int inv[8] = {
        BCDEC_IDENTITY, BCDEC_ROTATE_270, BCDEC_ROTATE_180,
        BCDEC_ROTATE_90, BCDEC_FLIP_X, BCDEC_FLIP_Y,
        BCDEC_TRANSPOSE, BCDEC_ANTI_TRANSPOSE
    };
    return inv[op];
}

void mapXY(int x, int y, int op, int& ox, int& oy)
{
    switch (op) {
        case BCDEC_ROTATE_90:          ox = 3 - y; oy = x; break;
        case BCDEC_ROTATE_180:         ox = 3 - x; oy = 3 - y; break;
        case BCDEC_ROTATE_270:         ox = y;     oy = 3 - x; break;
        case BCDEC_FLIP_X:             ox = 3 - x; oy = y; break;
        case BCDEC_FLIP_Y:             ox = x;     oy = 3 - y; break;
        case BCDEC_TRANSPOSE:          ox = y;     oy = x; break;
        case BCDEC_ANTI_TRANSPOSE:     ox = 3 - y; oy = 3 - x; break;
        default:                       ox = x;     oy = y; break;
    }
}

struct BitWriter {
    BlockBytes b{};
    unsigned pos = 0;
    void put(uint32_t value, int n)
    {
        for (int k = 0; k < n; ++k, ++pos) {
            b[pos >> 3] |= static_cast<Byte>(((value >> k) & 1u) << (pos & 7u));
        }
    }
};

struct BitReader {
    const Byte* b;
    unsigned pos = 0;
    explicit BitReader(const Byte* bytes) : b(bytes) {}
    uint32_t get(int n)
    {
        uint32_t value = 0;
        for (int k = 0; k < n; ++k, ++pos) {
            value |= uint32_t((b[pos >> 3] >> (pos & 7u)) & 1u) << k;
        }
        return value;
    }
};

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct TestBlock {
    int mode = 6;
    int partition = 0;
    int rotation = 0;
    int selection = 0;
    uint32_t ep[6][4]{};
    uint32_t pbit[6]{};
    uint32_t sharedPbit[2]{};
    uint32_t idx[2][4][4]{};

    bool anchor(int y, int x) const
    {
        int s = subsetCount(mode);
        if (s == 1) return x == 0 && y == 0;
        return (bcdec_bc7_partition_sets[s - 2][partition][y][x] & 0x80) != 0;
    }

    BlockBytes write() const
    {
        BitWriter w;
        for (int i = 0; i < mode; ++i) w.put(0, 1);
        w.put(1, 1);

        int pb = partitionBits(mode);
        if (pb) w.put(uint32_t(partition), pb);
        if (mode == 4 || mode == 5) {
            w.put(uint32_t(rotation), 2);
            if (mode == 4) w.put(uint32_t(selection), 1);
        }

        int s = subsetCount(mode);
        int n = 2 * s;
        int cb = colorBits(mode);
        int ab = alphaBits(mode);

        for (int c = 0; c < 3; ++c)
            for (int e = 0; e < n; ++e)
                w.put(ep[e][c], cb);
        if (ab)
            for (int e = 0; e < n; ++e)
                w.put(ep[e][3], ab);

        if (mode == 1) {
            w.put(sharedPbit[0], 1);
            w.put(sharedPbit[1], 1);
        } else if (mode == 0 || mode == 3 || mode == 6 || mode == 7) {
            for (int e = 0; e < n; ++e) w.put(pbit[e], 1);
        }

        int w1 = indexBits1(mode);
        int w2 = indexBits2(mode);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                bool a = anchor(y, x);
                if (a) assert((idx[0][y][x] & (1u << (w1 - 1))) == 0);
                w.put(idx[0][y][x], w1 - int(a));
            }
        }
        if (w2) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    bool a = anchor(y, x);
                    if (a) assert((idx[1][y][x] & (1u << (w2 - 1))) == 0);
                    w.put(idx[1][y][x], w2 - int(a));
                }
            }
        }

        assert(w.pos == 128);
        return w.b;
    }

    static TestBlock read(const BlockBytes& bytes)
    {
        BitReader r(bytes.data());
        TestBlock b;
        for (b.mode = 0; b.mode < 8; ++b.mode)
            if (r.get(1)) break;
        assert(b.mode < 8);

        int pb = partitionBits(b.mode);
        if (pb) b.partition = int(r.get(pb));
        if (b.mode == 4 || b.mode == 5) {
            b.rotation = int(r.get(2));
            if (b.mode == 4) b.selection = int(r.get(1));
        }

        int s = subsetCount(b.mode);
        int n = 2 * s;
        int cb = colorBits(b.mode);
        int ab = alphaBits(b.mode);
        for (int c = 0; c < 3; ++c)
            for (int e = 0; e < n; ++e)
                b.ep[e][c] = r.get(cb);
        if (ab)
            for (int e = 0; e < n; ++e)
                b.ep[e][3] = r.get(ab);

        if (b.mode == 1) {
            b.sharedPbit[0] = r.get(1);
            b.sharedPbit[1] = r.get(1);
        } else if (b.mode == 0 || b.mode == 3 || b.mode == 6 || b.mode == 7) {
            for (int e = 0; e < n; ++e) b.pbit[e] = r.get(1);
        }

        int w1 = indexBits1(b.mode);
        int w2 = indexBits2(b.mode);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                b.idx[0][y][x] = r.get(w1 - int(b.anchor(y, x)));
        if (w2)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    b.idx[1][y][x] = r.get(w2 - int(b.anchor(y, x)));
        assert(r.pos == 128);
        return b;
    }
};

TestBlock patternedBlock(int mode, int partition, uint32_t seed)
{
    TestBlock b;
    b.mode = mode;
    b.partition = partition;
    int s = subsetCount(mode);

    b.rotation = int(hash32(seed + 901) & 3u);
    b.selection = int(hash32(seed + 902) & 1u);

    for (int sub = 0; sub < s; ++sub) {
        for (int c = 0; c < 4; ++c) {
            int bits = (c == 3) ? alphaBits(mode) : colorBits(mode);
            if (!bits) continue;
            uint32_t mask = (1u << bits) - 1u;
            uint32_t low = hash32(seed * 131u + uint32_t(sub) * 17u +
                                  uint32_t(c) * 7u + 3u) %
                           (1u << (bits - 1));
            b.ep[2 * sub][c] = low;
            b.ep[2 * sub + 1][c] = mask - low;
        }

        if (mode == 1) {
            b.sharedPbit[sub] = hash32(seed + uint32_t(sub) * 199u) & 1u;
        } else {
            b.pbit[2 * sub] = hash32(seed + uint32_t(sub) * 31u + 1u) & 1u;
            b.pbit[2 * sub + 1] = hash32(seed + uint32_t(sub) * 31u + 2u) & 1u;
        }
    }

    int w1 = indexBits1(mode);
    int w2 = indexBits2(mode);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            b.idx[0][y][x] =
                hash32(seed * 17u + uint32_t(y * 37 + x * 11 + mode * 101)) %
                (1u << w1);
            if (b.anchor(y, x))
                b.idx[0][y][x] &= ~(1u << (w1 - 1));

            if (w2) {
                b.idx[1][y][x] =
                    hash32(seed * 29u + uint32_t(y * 41 + x * 23 + mode * 103)) %
                    (1u << w2);
                if (b.anchor(y, x))
                    b.idx[1][y][x] &= ~(1u << (w2 - 1));
            }
        }
    }

    return b;
}

Pixels decodeBlock(const Byte* block)
{
    Pixels px{};
    bcdec_bc7(block, px.data(), 16);
    return px;
}

void checkPixelPermutation(Case& tc, const BlockBytes& src,
                           const BlockBytes& dst, int op,
                           const std::string& context)
{
    Pixels a = decodeBlock(src.data());
    Pixels b = decodeBlock(dst.data());

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int ox, oy;
            mapXY(x, y, op, ox, oy);
            for (int c = 0; c < 4; ++c) {
                Byte expected = a[(y * 4 + x) * 4 + c];
                Byte actual = b[(oy * 4 + ox) * 4 + c];
                std::string msg = context + ": channel " +
                                  std::to_string(c) + " source (" +
                                  std::to_string(x) + "," +
                                  std::to_string(y) + ") -> (" +
                                  std::to_string(ox) + "," +
                                  std::to_string(oy) + ") expected " +
                                  std::to_string(int(expected)) +
                                  " got " + std::to_string(int(actual));
                tc.check(expected == actual, msg.c_str());
            }
        }
    }
}

struct OracleLayout {
    bool representable = false;
    int partition = -1;
    int map[3] = { -1, -1, -1 };
};

OracleLayout findOracleLayout(int mode, int sourcePartition, int op)
{
    OracleLayout result;
    int s = subsetCount(mode);
    int transformed[4][4]{};

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int label = 0;
            if (s > 1)
                label = bcdec_bc7_partition_sets[s - 2][sourcePartition][y][x] & 3;
            int ox, oy;
            mapXY(x, y, op, ox, oy);
            transformed[oy][ox] = label;
        }
    }

    if (s == 1) {
        result.representable = true;
        result.partition = 0;
        result.map[0] = 0;
        return result;
    }

    int limit = (mode == 0) ? 16 : 64;
    for (int p = 0; p < limit; ++p) {
        std::vector<int> perm(s);
        std::iota(perm.begin(), perm.end(), 0);
        do {
            bool ok = true;
            for (int y = 0; y < 4 && ok; ++y) {
                for (int x = 0; x < 4; ++x) {
                    int targetLabel =
                        bcdec_bc7_partition_sets[s - 2][p][y][x] & 3;
                    if (transformed[y][x] != perm[targetLabel]) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) {
                result.representable = true;
                result.partition = p;
                for (int i = 0; i < s; ++i) result.map[i] = perm[i];
                return result;
            }
        } while (std::next_permutation(perm.begin(), perm.end()));
    }

    return result;
}

void expectSentinel(Case& tc, const BlockBytes& dst, const BlockBytes& sentinel,
                    const std::string& context)
{
    bool unchanged = std::memcmp(dst.data(), sentinel.data(), 16) == 0;
    tc.check(unchanged, (context + ": dst was modified on failure").c_str());
}

BlockBytes sentinelBytes()
{
    BlockBytes b;
    b.fill(0xA5);
    return b;
}

void modeTests(Case& tc)
{
    for (int mode = 0; mode < 8; ++mode) {
        int pid = partitionBits(mode) ? ((mode == 0) ? 14 : 37) : 0;

        for (uint32_t seed = 0; seed < 16; ++seed) {
            TestBlock tb = patternedBlock(mode, pid, seed * 17u + 3u);
            if (mode == 4 || mode == 5) tb.rotation = int(seed & 3u);
            if (mode == 4) tb.selection = int((seed >> 1) & 1u);
            BlockBytes src = tb.write();

            BlockBytes identity{};
            auto st = bcdec_bc7_transform(src.data(), identity.data(),
                                          BCDEC_IDENTITY);
            tc.check(st == BCDEC_TRANSFORM_OK, "mode identity failed");
            tc.check(identity == src, "IDENTITY changed original bytes");

            for (int rot = 0; rot < 4; ++rot) {
                if (mode != 4 && mode != 5) break;
                tb.rotation = rot;
                if (mode == 4) tb.selection = int(seed & 1u);
                src = tb.write();
                BlockBytes out{};
                st = bcdec_bc7_transform(src.data(), out.data(), BCDEC_IDENTITY);
                tc.check(st == BCDEC_TRANSFORM_OK && out == src,
                         "rotation/index-selection field identity failed");
            }
        }

        for (uint32_t seed = 0; seed < 5; ++seed) {
            TestBlock tb = patternedBlock(mode, pid, 700u + seed * 31u + mode);
            BlockBytes src = tb.write();

            for (int op = 0; op < OPS; ++op) {
                OracleLayout oracle = findOracleLayout(mode, pid, op);
                BlockBytes sentinel = sentinelBytes();
                BlockBytes dst = sentinel;
                auto st = bcdec_bc7_transform(src.data(), dst.data(),
                                              bcdec_transform_op(op));
                std::string ctx = "modes mode=" + std::to_string(mode) +
                                  " seed=" + std::to_string(seed) +
                                  " op=" + std::to_string(op);

                if (!oracle.representable) {
                    tc.check(st == BCDEC_TRANSFORM_UNREPRESENTABLE,
                             (ctx + ": expected UNREPRESENTABLE").c_str());
                    expectSentinel(tc, dst, sentinel, ctx);
                } else {
                    tc.check(st == BCDEC_TRANSFORM_OK,
                             (ctx + ": expected OK").c_str());
                    checkPixelPermutation(tc, src, dst, op, ctx);
                }
            }
        }
    }
}

void partitionTests(Case& tc)
{
    const int modes[] = { 0, 1, 2, 3, 7 };
    int totalLayouts = 0;
    int representableLayouts = 0;

    for (int mode : modes) {
        int limit = (mode == 0) ? 16 : 64;
        for (int pid = 0; pid < limit; ++pid) {
            for (int op = 0; op < OPS; ++op) {
                ++totalLayouts;
                OracleLayout oracle = findOracleLayout(mode, pid, op);
                if (oracle.representable) ++representableLayouts;

                TestBlock tb = patternedBlock(
                    mode, pid, uint32_t(mode * 10000 + pid * 17 + op * 3 + 1));
                BlockBytes src = tb.write();
                BlockBytes sentinel = sentinelBytes();
                BlockBytes dst = sentinel;
                auto st = bcdec_bc7_transform(src.data(), dst.data(),
                                              bcdec_transform_op(op));

                std::string ctx = "partition mode=" + std::to_string(mode) +
                                  " pid=" + std::to_string(pid) +
                                  " op=" + std::to_string(op);

                if (!oracle.representable) {
                    tc.check(st == BCDEC_TRANSFORM_UNREPRESENTABLE,
                             (ctx + ": expected structural UNREPRESENTABLE")
                                 .c_str());
                    expectSentinel(tc, dst, sentinel, ctx);
                    continue;
                }

                tc.check(st == BCDEC_TRANSFORM_OK,
                         (ctx + ": expected representable transform").c_str());
                TestBlock out = TestBlock::read(dst);
                tc.check(out.partition == oracle.partition,
                         (ctx + ": wrong tie-break partition, expected " +
                          std::to_string(oracle.partition) + " got " +
                          std::to_string(out.partition))
                             .c_str());
                checkPixelPermutation(tc, src, dst, op, ctx);
            }
        }
    }

    /* The task explicitly requires this structural counterexample. */
    OracleLayout p14r180 = findOracleLayout(0, 14, BCDEC_ROTATE_180);
    tc.check(!p14r180.representable,
             "oracle must find mode0 partition14 ROTATE_180 unrepresentable");

    std::fprintf(stderr,
                 "partition enumeration: %d mode/partition/op layouts, %d representable\n",
                 totalLayouts, representableLayouts);
}

void expectIndexArrays(Case& tc, const TestBlock& original,
                       const TestBlock& transformed, int op,
                       const int swap[2], const std::string& ctx)
{
    int widths[2] = { indexBits1(original.mode), indexBits2(original.mode) };
    for (int stream = 0; stream < 2; ++stream) {
        if (!widths[stream]) continue;
        uint32_t mask = (1u << widths[stream]) - 1u;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                int ox, oy;
                mapXY(x, y, op, ox, oy);
                uint32_t expected = original.idx[stream][y][x];
                if (swap[stream]) expected ^= mask;
                uint32_t actual = transformed.idx[stream][oy][ox];
                tc.check(actual == expected,
                         (ctx + ": index stream " + std::to_string(stream) +
                          " mismatch at destination " + std::to_string(ox) +
                          "," + std::to_string(oy))
                             .c_str());
            }
        }
    }
}

void anchorTests(Case& tc)
{
    /*
     * Fixed one-stream counterexample.  FLIP_Y sends source texel (0,3) to
     * the destination anchor (0,0).  Its 4-bit index is 8, whose MSB is one.
     * The anchor can only omit a zero MSB, so the endpoint pair must swap and
     * every index in that subset must reverse.
     */
    {
        TestBlock b;
        b.mode = 6;
        for (int c = 0; c < 3; ++c) {
            b.ep[0][c] = uint32_t(10 + c * 11);
            b.ep[1][c] = uint32_t(110 - c * 13);
        }
        b.ep[0][3] = 33;
        b.ep[1][3] = 122;
        b.pbit[0] = 1;
        b.pbit[1] = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                b.idx[0][y][x] = uint32_t((x * 5 + y * 7 + 1) & 15);
        b.idx[0][0][0] = 2; /* source anchor: omitted MSB is zero */
        b.idx[0][3][0] = 8; /* becomes destination anchor, MSB is one */

        BlockBytes src = b.write();
        tc.check((b.idx[0][3][0] & 8u) != 0,
                 "test setup must really move an index MSB=1 to an anchor");
        tc.check((b.idx[0][3][0] & 7u) == 0,
                 "naive truncation would destroy index 8");

        BlockBytes dst{};
        auto st = bcdec_bc7_transform(src.data(), dst.data(), BCDEC_FLIP_Y);
        tc.check(st == BCDEC_TRANSFORM_OK, "mode6 anchor counterexample failed");

        TestBlock out = TestBlock::read(dst);
        tc.check(out.idx[0][0][0] == 7,
                 "endpoint reversal must change anchor index 8 to 7");
        tc.check(out.ep[0][0] == b.ep[1][0] &&
                     out.ep[0][1] == b.ep[1][1] &&
                     out.ep[0][2] == b.ep[1][2] &&
                     out.ep[0][3] == b.ep[1][3] &&
                     out.ep[1][0] == b.ep[0][0] &&
                     out.ep[1][1] == b.ep[0][1] &&
                     out.ep[1][2] == b.ep[0][2] &&
                     out.ep[1][3] == b.ep[0][3],
                 "mode6 endpoints/pbits must swap together");
        tc.check(out.pbit[0] == b.pbit[1] && out.pbit[1] == b.pbit[0],
                 "mode6 p-bits must follow their endpoints");
        int swap[2] = { 1, 0 };
        expectIndexArrays(tc, b, out, BCDEC_FLIP_Y, swap, "mode6 anchor");
        checkPixelPermutation(tc, src, dst, BCDEC_FLIP_Y, "mode6 anchor");
    }

    /* Mode 4, index-selection bit zero: primary=RGB, secondary=alpha. */
    for (int variant = 0; variant < 2; ++variant) {
        TestBlock b;
        b.mode = 4;
        b.selection = 0;
        b.rotation = 2;
        b.ep[0][0] = 3;  b.ep[0][1] = 9;  b.ep[0][2] = 20; b.ep[0][3] = 11;
        b.ep[1][0] = 28; b.ep[1][1] = 21; b.ep[1][2] = 7;  b.ep[1][3] = 50;

        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                b.idx[0][y][x] = uint32_t((x + y) & 3);
                b.idx[1][y][x] = hash32(uint32_t(x * 13 + y * 17 + variant)) & 7u;
                if (b.anchor(y, x)) {
                    b.idx[0][y][x] &= 1u;
                    b.idx[1][y][x] &= 3u;
                }
            }
        }
        b.idx[0][0][0] = 1;
        b.idx[1][0][0] = 2;
        if (variant == 0) {
            /* Primary MSB=1 forces RGB swap; secondary MSB=0 leaves alpha. */
            b.idx[0][3][0] = 2;
            b.idx[1][3][0] = 1;
        } else {
            /* Primary MSB=0 leaves RGB; secondary MSB=1 forces alpha swap. */
            b.idx[0][3][0] = 1;
            b.idx[1][3][0] = 4;
        }

        BlockBytes src = b.write();
        BlockBytes dst{};
        auto st = bcdec_bc7_transform(src.data(), dst.data(), BCDEC_FLIP_Y);
        tc.check(st == BCDEC_TRANSFORM_OK, "mode4 dual-stream transform failed");
        TestBlock out = TestBlock::read(dst);

        tc.check(out.selection == 0 && out.rotation == 2,
                 "mode4 selection/rotation bits must be preserved");

        int colorSwap = (variant == 0);
        int alphaSwap = (variant == 1);
        tc.check(int(out.ep[0][0] == b.ep[colorSwap ? 1 : 0][0] &&
                     out.ep[0][1] == b.ep[colorSwap ? 1 : 0][1] &&
                     out.ep[0][2] == b.ep[colorSwap ? 1 : 0][2]) == 1,
                 "mode4 RGB endpoint direction does not match color anchor");
        tc.check(int(out.ep[0][3] == b.ep[alphaSwap ? 1 : 0][3] &&
                     out.ep[1][3] == b.ep[alphaSwap ? 0 : 1][3]) == 1,
                 "mode4 alpha endpoint direction does not match alpha anchor");

        int swaps[2] = { colorSwap, alphaSwap };
        expectIndexArrays(tc, b, out, BCDEC_FLIP_Y, swaps,
                          "mode4 selection=0 stream separation");
        checkPixelPermutation(tc, src, dst, BCDEC_FLIP_Y,
                              "mode4 selection=0 stream separation");
    }

    /* Mode 4 with selection=1: physical secondary controls RGB. */
    {
        TestBlock b;
        b.mode = 4;
        b.selection = 1;
        b.rotation = 3;
        b.ep[0][0] = 4;  b.ep[0][1] = 12; b.ep[0][2] = 19; b.ep[0][3] = 40;
        b.ep[1][0] = 30; b.ep[1][1] = 25; b.ep[1][2] = 31; b.ep[1][3] = 59;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                b.idx[0][y][x] = hash32(uint32_t(x + y * 9)) & 3u;
                b.idx[1][y][x] = hash32(uint32_t(x * 7 + y * 5)) & 7u;
                if (b.anchor(y, x)) {
                    b.idx[0][y][x] &= 1u;
                    b.idx[1][y][x] &= 3u;
                }
            }
        }
        b.idx[0][3][0] = 1; /* alpha-controlled primary, MSB zero */
        b.idx[1][3][0] = 4; /* RGB-controlled secondary, MSB one */

        BlockBytes src = b.write();
        BlockBytes dst{};
        auto st = bcdec_bc7_transform(src.data(), dst.data(), BCDEC_FLIP_Y);
        tc.check(st == BCDEC_TRANSFORM_OK, "mode4 selection=1 failed");
        TestBlock out = TestBlock::read(dst);

        tc.check(out.selection == 1, "mode4 selection bit changed");
        tc.check(out.ep[0][0] == b.ep[1][0] && out.ep[0][1] == b.ep[1][1] &&
                     out.ep[0][2] == b.ep[1][2],
                 "selection=1 must reverse RGB using secondary anchor");
        tc.check(out.ep[0][3] == b.ep[0][3] && out.ep[1][3] == b.ep[1][3],
                 "selection=1 alpha direction must follow primary anchor");
        int swaps[2] = { 0, 1 };
        expectIndexArrays(tc, b, out, BCDEC_FLIP_Y, swaps,
                          "mode4 selection=1 stream roles");
        checkPixelPermutation(tc, src, dst, BCDEC_FLIP_Y,
                              "mode4 selection=1 stream roles");
    }

    /* Mode 5 has two equal-width but still independent streams. */
    for (int variant = 0; variant < 2; ++variant) {
        TestBlock b;
        b.mode = 5;
        b.rotation = variant ? 1 : 0;
        b.ep[0][0] = 7;  b.ep[0][1] = 17; b.ep[0][2] = 27; b.ep[0][3] = 41;
        b.ep[1][0] = 120; b.ep[1][1] = 90; b.ep[1][2] = 60; b.ep[1][3] = 200;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                b.idx[0][y][x] = hash32(uint32_t(x * 3 + y * 5 + variant)) & 3u;
                b.idx[1][y][x] = hash32(uint32_t(x * 11 + y * 7 + variant)) & 3u;
                if (b.anchor(y, x)) {
                    b.idx[0][y][x] &= 1u;
                    b.idx[1][y][x] &= 1u;
                }
            }
        }
        if (variant == 0) {
            b.idx[0][3][0] = 2;
            b.idx[1][3][0] = 1;
        } else {
            b.idx[0][3][0] = 1;
            b.idx[1][3][0] = 2;
        }

        BlockBytes src = b.write();
        BlockBytes dst{};
        auto st = bcdec_bc7_transform(src.data(), dst.data(), BCDEC_FLIP_Y);
        tc.check(st == BCDEC_TRANSFORM_OK, "mode5 stream test failed");
        TestBlock out = TestBlock::read(dst);
        int swaps[2] = { variant == 0, variant == 1 };
        expectIndexArrays(tc, b, out, BCDEC_FLIP_Y, swaps, "mode5 streams");
        checkPixelPermutation(tc, src, dst, BCDEC_FLIP_Y, "mode5 streams");
    }
}

Image8x8 decode8x8(const BlockBytes blocks[2][2])
{
    Image8x8 image{};
    for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
            Byte decoded[128];
            bcdec_bc7(blocks[by][bx].data(), decoded, 32);
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    int gx = bx * 4 + x;
                    int gy = by * 4 + y;
                    for (int c = 0; c < 4; ++c)
                        image[(gy * 8 + gx) * 4 + c] = decoded[y * 32 + x * 4 + c];
                }
            }
        }
    }
    return image;
}

void writePPM(const std::string& path, const Image8x8& rgba)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("could not write PPM: " + path);
    std::fprintf(f, "P6\n8 8\n255\n");
    for (int i = 0; i < 64; ++i)
        std::fwrite(&rgba[size_t(i) * 4], 1, 3, f);
    std::fclose(f);
}

void integrationTests(Case& tc)
{
    /* Null pointers and unknown operations are INVALID. */
    BlockBytes dummy{};
    tc.check(bcdec_bc7_transform(nullptr, dummy.data(), BCDEC_IDENTITY) ==
                 BCDEC_TRANSFORM_INVALID,
             "null src must be INVALID");
    BlockBytes src = patternedBlock(6, 0, 42).write();
    tc.check(bcdec_bc7_transform(src.data(), nullptr, BCDEC_IDENTITY) ==
                 BCDEC_TRANSFORM_INVALID,
             "null dst must be INVALID");
    tc.check(bcdec_bc7_transform(src.data(), dummy.data(),
                                 static_cast<bcdec_transform_op>(99)) ==
                 BCDEC_TRANSFORM_INVALID,
             "unknown op must be INVALID");

    BlockBytes zero{};
    BlockBytes sentinel = sentinelBytes();
    BlockBytes guarded = sentinel;
    tc.check(bcdec_bc7_transform(zero.data(), guarded.data(),
                                 BCDEC_ROTATE_90) == BCDEC_TRANSFORM_INVALID,
             "mode-less block must be INVALID");
    expectSentinel(tc, guarded, sentinel, "invalid mode");

    /* The required mode0 partition14 ROTATE_180 structural counterexample. */
    BlockBytes bad = patternedBlock(0, 14, 1234).write();
    guarded = sentinel;
    tc.check(bcdec_bc7_transform(bad.data(), guarded.data(),
                                 BCDEC_ROTATE_180) ==
                 BCDEC_TRANSFORM_UNREPRESENTABLE,
             "mode0 partition14 ROTATE_180 must be UNREPRESENTABLE");
    expectSentinel(tc, guarded, sentinel, "structural failure");

    /* In-place operation and transform/inverse pixel round trips. */
    for (int mode = 0; mode < 8; ++mode) {
        int pid = partitionBits(mode) ? ((mode == 0) ? 14 : 53) : 0;
        for (uint32_t seed = 1; seed <= 3; ++seed) {
            BlockBytes original =
                patternedBlock(mode, pid, seed * 977u + uint32_t(mode)).write();

            for (int op = 0; op < OPS; ++op) {
                OracleLayout oracle = findOracleLayout(mode, pid, op);
                if (!oracle.representable) continue;

                BlockBytes inPlace = original;
                auto st = bcdec_bc7_transform(inPlace.data(), inPlace.data(),
                                              bcdec_transform_op(op));
                tc.check(st == BCDEC_TRANSFORM_OK, "in-place transform failed");
                checkPixelPermutation(tc, original, inPlace, op,
                                      "in-place integration");

                BlockBytes repeated{};
                st = bcdec_bc7_transform(original.data(), repeated.data(),
                                         bcdec_transform_op(op));
                tc.check(st == BCDEC_TRANSFORM_OK && repeated == inPlace,
                         "repeated transform run is not stable");

                BlockBytes roundTrip{};
                st = bcdec_bc7_transform(inPlace.data(), roundTrip.data(),
                                         bcdec_transform_op(inverseOp(op)));
                tc.check(st == BCDEC_TRANSFORM_OK, "inverse transform failed");
                Pixels a = decodeBlock(original.data());
                Pixels b = decodeBlock(roundTrip.data());
                tc.check(a == b, "transform followed by inverse changed pixels");
            }
        }
    }

    /*
     * Four distinct mode6 blocks form an 8x8 texture.  Mode6 is single-subset
     * and therefore always structurally transformable.  The block-coordinate
     * formulas below are the 8x8 versions of the local 4x4 formulas.
     */
    BlockBytes sourceBlocks[2][2];
    for (int by = 0; by < 2; ++by)
        for (int bx = 0; bx < 2; ++bx)
            sourceBlocks[by][bx] =
                patternedBlock(6, 0, uint32_t(100 + by * 20 + bx * 5)).write();

    Image8x8 source8 = decode8x8(sourceBlocks);

    for (int op = 0; op < OPS; ++op) {
        BlockBytes outBlocks[2][2]{};
        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                int dbx = bx, dby = by;
                switch (op) {
                    case BCDEC_ROTATE_90:      dbx = 1 - by; dby = bx; break;
                    case BCDEC_ROTATE_180:     dbx = 1 - bx; dby = 1 - by; break;
                    case BCDEC_ROTATE_270:     dbx = by;     dby = 1 - bx; break;
                    case BCDEC_FLIP_X:         dbx = 1 - bx; dby = by; break;
                    case BCDEC_FLIP_Y:         dbx = bx;     dby = 1 - by; break;
                    case BCDEC_TRANSPOSE:      dbx = by;     dby = bx; break;
                    case BCDEC_ANTI_TRANSPOSE: dbx = 1 - by; dby = 1 - bx; break;
                    default: break;
                }

                auto st = bcdec_bc7_transform(
                    sourceBlocks[by][bx].data(),
                    outBlocks[dby][dbx].data(),
                    bcdec_transform_op(op));
                tc.check(st == BCDEC_TRANSFORM_OK, "8x8 block transform failed");
            }
        }

        Image8x8 actual = decode8x8(outBlocks);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int ox = x, oy = y;
                switch (op) {
                    case BCDEC_ROTATE_90:      ox = 7 - y; oy = x; break;
                    case BCDEC_ROTATE_180:     ox = 7 - x; oy = 7 - y; break;
                    case BCDEC_ROTATE_270:     ox = y;     oy = 7 - x; break;
                    case BCDEC_FLIP_X:         ox = 7 - x; oy = y; break;
                    case BCDEC_FLIP_Y:         ox = x;     oy = 7 - y; break;
                    case BCDEC_TRANSPOSE:      ox = y;     oy = x; break;
                    case BCDEC_ANTI_TRANSPOSE: ox = 7 - y; oy = 7 - x; break;
                    default: break;
                }
                for (int c = 0; c < 4; ++c) {
                    tc.check(actual[(oy * 8 + ox) * 4 + c] ==
                                 source8[(y * 8 + x) * 4 + c],
                             "8x8 block placement/internal permutation mismatch");
                }
            }
        }

        if (op == BCDEC_ROTATE_90) {
            writePPM("task_tests/target.ppm", actual);
        }
    }
}

} /* namespace */

void register_task_tests(Registry& registry)
{
    registry.add("modes", "all-eight-bc7-modes", [](Case& tc) { modeTests(tc); });
    registry.add("partitions", "exhaustive-standard-partitions",
                 [](Case& tc) { partitionTests(tc); });
    registry.add("anchors", "fixup-and-dual-stream-cases",
                 [](Case& tc) { anchorTests(tc); });
    registry.add("integration", "api-roundtrip-and-8x8-atlas",
                 [](Case& tc) { integrationTests(tc); });
}
