// golomb_contract.cpp
//
// Stream-compatibility contract for the ALAC adaptive-Golomb layer.
//
// This file only defines task_group(); the fixed driver task_support/main.c
// supplies main()/task_check().  The three groups are:
//
//   codes   - signed residual plan -> independent MSB-first bit model,
//             compared in both directions against production dyn_comp /
//             dyn_decomp output (per residual, bit string, consumed bits),
//             with front/back sentinels and start bitIndex 0..7.
//   runs    - mean/k/zmode evolution, zero runs of length 0..65535, the
//             post-run sign correction, bit positions, block reset and the
//             run-overrun sample-count error with untouched output sentinel.
//   packets - independently written compressed SCE/CPE streams fed to
//             ALACDecoder::Decode, covering 16/24-bit, partial frames,
//             24-bit shift-off, adjacent elements and consecutive packets,
//             plus a frozen production-encoder compressed packet.
//
// Reference expectations come from tests/golomb_model.h only; this file
// calls the production dyn_comp/dyn_decomp/ALACDecoder entries as the
// systems under test and uses production BitBuffer just to hand arrays over.

#include "task_support/check.h"

#include "golomb_model.h"
#include "golomb_image.h"
#include "golomb_packets.h"
#include "encoder_compat_material.h"

#include "aglib.h"
#include "ALACBitUtilities.h"
#include "ALACAudioTypes.h"
#include "ALACDecoder.h"

#include <vector>
#include <cstring>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static uint32_t prefillBit(size_t absPos) {
    // gi::kStreamFill (0xA5) repeated over stream + padding.
    return (gi::kStreamFill >> (7u - (absPos & 7u))) & 1u;
}

static AGParamRec makeAG(uint32_t mb, uint32_t pb, uint32_t kb, uint32_t n) {
    AGParamRec ap;
    set_ag_params(&ap, mb, pb, kb, n, n, 255);
    return ap;
}

// ===========================================================================
// codes group
// ===========================================================================
struct CodesCoverage {
    bool k1 = false;
    bool zeroRemainder = false;
    bool nonZeroRemainder = false;
    bool escape = false;
    bool shortCode = false;
    bool w16 = false, w20 = false, w24 = false;
    bool start[8] = {false,false,false,false,false,false,false,false};
};

static void codesScenario(const std::vector<int32_t>& plan,
                          uint32_t mb, uint32_t pb, uint32_t kb,
                          uint32_t bitsz, uint32_t startBit,
                          CodesCoverage& cov) {
    const uint32_t N = (uint32_t)plan.size();

    // ---- independent expectation ----------------------------------------
    gm::Block mdl = gm::encodePlan(plan, mb, pb, kb, bitsz);
    TASK_CHECK(mdl.runs.empty(), "codes: high-mean plan must not arm zmode");

    for (size_t i = 0; i < mdl.mains.size(); ++i) {
        const gm::MainSym& s = mdl.mains[i];
        if (s.code.k == 1) cov.k1 = true;
        if (s.code.escape) {
            cov.escape = true;
        } else {
            cov.shortCode = true;
            if (s.code.zeroRemainder) cov.zeroRemainder = true;
            else                      cov.nonZeroRemainder = true;
        }
    }
    if (bitsz == 16) cov.w16 = true;
    if (bitsz == 20) cov.w20 = true;
    if (bitsz == 24) cov.w24 = true;
    cov.start[startBit & 7u] = true;

    size_t logicalBytes = (startBit + mdl.encoded.size() + 7u) / 8u + 16u;

    // ---- forward: production dyn_comp must equal the independent bits ----
    gi::Image ci(logicalBytes);
    std::vector<uint8_t> packed =
        gm::packAt(mdl.encoded.b, startBit, logicalBytes, gi::kStreamFill);
    std::memcpy(ci.p, packed.data(), logicalBytes);

    AGParamRec ap = makeAG(mb, pb, kb, N);
    BitBuffer bc;
    BitBufferInit(&bc, ci.p, (uint32_t)logicalBytes);
    bc.bitIndex = startBit;

    uint32_t prodBits = 0xdeadbeefu;
    int cst = dyn_comp(&ap, const_cast<int32_t*>(plan.data()), &bc,
                       (int32_t)N, bitsz, &prodBits);
    TASK_CHECK(cst == 0, "codes: dyn_comp returned status");
    TASK_CHECK((size_t)prodBits == mdl.encoded.size(),
               "codes: forward consumed bit count");

    gm::ByteReader prodReader(ci.p, ci.readableBytes());
    for (size_t i = 0; i < mdl.encoded.size(); ++i) {
        TASK_CHECK((uint32_t)prodReader.bitAt(startBit + i) == (uint32_t)mdl.encoded.b[i],
                   "codes: forward effective bitstring");
    }

    // front bit sentinel (unused high bits of byte 0) and post-window bits
    for (uint32_t b = 0; b < startBit; ++b)
        TASK_CHECK((uint32_t)prodReader.bitAt(b) == prefillBit(b),
                   "codes: front bit sentinel preserved");
    for (uint32_t j = 0; j < 32; ++j) {
        size_t pos = (size_t)startBit + prodBits + j;
        TASK_CHECK((uint32_t)prodReader.bitAt(pos) == prefillBit(pos),
                   "codes: post bit sentinel preserved");
    }
    TASK_CHECK(ci.hardGuardsIntact(), "codes: hard byte sentinels after jam");

    // ---- production dyn_decomp of production dyn_comp bits ---------------
    gi::Image di(logicalBytes);
    std::memcpy(di.p, ci.p, logicalBytes);
    std::vector<int32_t> decoded(N + 8, (int32_t)0x3c3c3c3c);

    ap = makeAG(mb, pb, kb, N);
    BitBuffer bd;
    BitBufferInit(&bd, di.p, (uint32_t)logicalBytes);
    bd.bitIndex = startBit;

    uint32_t decBits = 0;
    int dst = dyn_decomp(&ap, &bd, decoded.data(), (int32_t)N, bitsz, &decBits);

    // Per-residual comparison first: this is the assertion that reports the
    // first content difference for the remainder/zmode mutations.
    for (uint32_t i = 0; i < N; ++i)
        TASK_CHECK(decoded[i] == plan[i], "codes: dyn_decomp per-residual");
    TASK_CHECK((size_t)decBits == mdl.encoded.size(),
               "codes: dyn_decomp consumed bit count");
    TASK_CHECK(dst == 0, "codes: dyn_decomp status");

    // ---- independent parser of production dyn_comp output ---------------
    gm::Block parsed = gm::decodeBlock(ci.p, ci.readableBytes(), startBit, N,
                                       mb, pb, kb, bitsz, logicalBytes);
    TASK_CHECK(parsed.status == gm::STATUS_OK, "codes: model parse status");
    TASK_CHECK(parsed.consumed == (size_t)prodBits,
               "codes: model parse consumed bit count");
    TASK_CHECK(parsed.out.size() == N, "codes: model parse sample count");
    for (uint32_t i = 0; i < N && i < parsed.out.size(); ++i)
        TASK_CHECK(parsed.out[i] == plan[i], "codes: model parse per-residual");

    // ---- independent bitstring fed to production dyn_decomp --------------
    gi::Image mi(logicalBytes);
    std::memcpy(mi.p, packed.data(), logicalBytes);
    std::vector<int32_t> crossed(N + 8, (int32_t)0x4d4d4d4d);
    ap = makeAG(mb, pb, kb, N);
    BitBuffer bm;
    BitBufferInit(&bm, mi.p, (uint32_t)logicalBytes);
    bm.bitIndex = startBit;
    uint32_t crossBits = 0;
    int mst = dyn_decomp(&ap, &bm, crossed.data(), (int32_t)N, bitsz, &crossBits);
    TASK_CHECK(mst == 0, "codes: model bits -> dyn_decomp status");
    TASK_CHECK((size_t)crossBits == mdl.encoded.size(),
               "codes: model bits -> dyn_decomp bit count");
    for (uint32_t i = 0; i < N; ++i)
        TASK_CHECK(crossed[i] == plan[i],
                   "codes: model bits -> dyn_decomp per-residual");
    TASK_CHECK(mi.hardGuardsIntact(), "codes: hard byte sentinels after decode");
}

static int codesGroup() {
    CodesCoverage cov;

    // Rich k=3 / m=7 plan: zero and non-zero remainders, short codes and
    // nine-one escape interleaved.  Run at every start bitIndex 0..7.
    {
        const uint32_t ns[] = {0, 1, 6, 7, 8, 13, 14, 21, 22,
                               63, 3, 70, 2, 49, 55, 56};
        std::vector<int32_t> plan;
        for (uint32_t n : ns)
            plan.push_back((n & 1u) ? -(int32_t)((n + 1u) / 2u)
                                    : (int32_t)(n / 2u));
        for (uint32_t s = 0; s < 8; ++s)
            codesScenario(plan, /*mb0*/2560, /*pb*/0, /*kb*/14, 16, s, cov);
    }

    // Explicit k=1 (m=1, remainder always zero), short and escape.
    // mb=256 keeps mb>>9 == 0 (so k=1) yet mb >= 128 (so zmode never arms).
    {
        const uint32_t ns[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 20};
        std::vector<int32_t> plan;
        for (uint32_t n : ns)
            plan.push_back((n & 1u) ? -(int32_t)((n + 1u) / 2u)
                                    : (int32_t)(n / 2u));
        codesScenario(plan, 256, 0, 14, 16, 0, cov);
        codesScenario(plan, 256, 0, 14, 16, 7, cov);
    }

    // 16/20/24-bit direct store after nine ones, at worst-case alignment.
    codesScenario({2, -24440}, 2560, 0, 14, 16, 7, cov);       // n=0xBEEF
    codesScenario({2, 351855}, 2560, 0, 14, 20, 6, cov);       // n=0xABCDE
    codesScenario({2, -5629688}, 2560, 0, 14, 24, 5, cov);     // n=0xABCDEF

    TASK_CHECK(cov.k1, "codes coverage: k=1 hit");
    TASK_CHECK(cov.zeroRemainder, "codes coverage: zero remainder hit");
    TASK_CHECK(cov.nonZeroRemainder, "codes coverage: non-zero remainder hit");
    TASK_CHECK(cov.escape, "codes coverage: nine-one escape hit");
    TASK_CHECK(cov.shortCode, "codes coverage: short code hit");
    TASK_CHECK(cov.w16 && cov.w20 && cov.w24,
               "codes coverage: 16/20/24 direct widths hit");
    for (uint32_t s = 0; s < 8; ++s)
        TASK_CHECK(cov.start[s], "codes coverage: start bitIndex 0..7 hit");

    return 1;
}

// ===========================================================================
// runs group
// ===========================================================================
static void runsCase(uint32_t runLength, int32_t target) {
    std::vector<int32_t> plan;
    plan.push_back(4);                       // anchor (normal mapping)
    for (uint32_t z = 0; z < runLength; ++z) plan.push_back(0);
    plan.push_back(target);                  // first post-run non-zero

    gm::Block mdl = gm::encodePlan(plan, 10, 0, 14, 16);
    TASK_CHECK(mdl.runs.size() >= 1, "runs: model emitted run");
    TASK_CHECK(mdl.runs[0].length == runLength, "runs: modelled run length");

    const uint32_t N = (uint32_t)plan.size();
    size_t logicalBytes = (mdl.encoded.size() + 64) / 8 + 16;

    gi::Image im(logicalBytes);
    std::vector<uint8_t> packed =
        gm::packAt(mdl.encoded.b, 0, logicalBytes, gi::kStreamFill);
    std::memcpy(im.p, packed.data(), logicalBytes);

    std::vector<int32_t> out(N + 8, (int32_t)0x5a5a5a5a);
    AGParamRec ap = makeAG(10, 0, 14, N);
    BitBuffer bb;
    BitBufferInit(&bb, im.p, (uint32_t)logicalBytes);
    uint32_t consumed = 0;
    int st = dyn_decomp(&ap, &bb, out.data(), (int32_t)N, 16, &consumed);

    // Mapping is checked sample-by-sample; the post-run target is the exact
    // first difference if ndecode drops the +zmode correction.
    for (uint32_t i = 0; i < N; ++i)
        TASK_CHECK(out[i] == plan[i], "runs: sample mapping after run");
    TASK_CHECK(st == 0, "runs: dyn_decomp status");
    TASK_CHECK((size_t)consumed == mdl.consumed, "runs: consumed bit count");
    TASK_CHECK((uint32_t)BitBufferGetPosition(&bb) == (uint32_t)mdl.consumed,
               "runs: bit position after block");

    // Independent parser agrees.
    gm::Block parsed = gm::decodeBlock(im.p, im.readableBytes(), 0, N,
                                       10, 0, 14, 16, logicalBytes);
    TASK_CHECK(parsed.status == 0 && parsed.consumed == mdl.consumed,
               "runs: model parse status/count");
    for (uint32_t i = 0; i < N && i < parsed.out.size(); ++i)
        TASK_CHECK(parsed.out[i] == plan[i], "runs: model parse sample");

    // Exact bit position of the next (post-run) symbol: decode the prefix
    // [anchor + L zeros] which ends immediately before the target.  Only
    // L > 0 emits that terminal run when it is the end of the prefix.
    if (runLength > 0) {
        size_t targetStart = mdl.mains.back().code.bitStart;
        std::vector<int32_t> prefix = plan;
        prefix.pop_back();
        gm::Block mp = gm::encodePlan(prefix, 10, 0, 14, 16);
        TASK_CHECK(mp.consumed == targetStart,
                   "runs: model target bitStart equals run end");

        gi::Image pi(logicalBytes);
        std::vector<uint8_t> pp =
            gm::packAt(mp.encoded.b, 0, logicalBytes, gi::kStreamFill);
        std::memcpy(pi.p, pp.data(), logicalBytes);
        uint32_t PN = (uint32_t)prefix.size();
        std::vector<int32_t> pout(PN + 8, (int32_t)0x5a5a5a5a);
        AGParamRec ap2 = makeAG(10, 0, 14, PN);
        BitBuffer pb;
        BitBufferInit(&pb, pi.p, (uint32_t)logicalBytes);
        uint32_t pcons = 0;
        int pst = dyn_decomp(&ap2, &pb, pout.data(), (int32_t)PN, 16, &pcons);
        TASK_CHECK(pst == 0, "runs: prefix status");
        TASK_CHECK((size_t)pcons == targetStart,
                   "runs: next-symbol bit position");
    }
}

static void runsAdaptiveDecayCase() {
    // Start from a high mean and let small non-zeros decay it below the
    // zmode arming threshold, then exercise runs + signed follow values.
    std::vector<int32_t> plan;
    for (uint32_t i = 0; i < 80; ++i) plan.push_back((i & 1u) ? -1 : 1);
    const uint32_t lengths[] = {0, 1, 5, 33, 300};
    const int32_t afters[] = {2, -2, 3, -3, 2};
    for (uint32_t c = 0; c < 5; ++c) {
        for (uint32_t z = 0; z < lengths[c]; ++z) plan.push_back(0);
        plan.push_back(afters[c]);
    }
    const uint32_t N = (uint32_t)plan.size();

    gm::Block mdl = gm::encodePlan(plan, /*mb0*/5000, gm::kPb0, gm::kKb0, 16);
    TASK_CHECK(!mdl.runs.empty(), "runs(adaptive): zmode runs emitted");
    bool decayed = false;
    for (size_t i = 0; i < mdl.mains.size(); ++i) {
        if (i > 4 && mdl.mains[i].meanBefore < 128u) decayed = true;
    }
    TASK_CHECK(decayed, "runs(adaptive): mean decayed below arming threshold");

    size_t logicalBytes = (mdl.encoded.size() + 128) / 8 + 16;
    gi::Image im(logicalBytes);
    auto packed = gm::packAt(mdl.encoded.b, 0, logicalBytes, gi::kStreamFill);
    std::memcpy(im.p, packed.data(), logicalBytes);

    std::vector<int32_t> out(N + 8, (int32_t)0x6b6b6b6b);
    AGParamRec ap = makeAG(5000, gm::kPb0, gm::kKb0, N);
    BitBuffer bb;
    BitBufferInit(&bb, im.p, (uint32_t)logicalBytes);
    uint32_t cons = 0;
    int st = dyn_decomp(&ap, &bb, out.data(), (int32_t)N, 16, &cons);
    TASK_CHECK(st == 0, "runs(adaptive): status");
    TASK_CHECK((size_t)cons == mdl.consumed, "runs(adaptive): bit count");
    for (uint32_t i = 0; i < N; ++i)
        TASK_CHECK(out[i] == plan[i], "runs(adaptive): per-sample");

    gm::Block parsed = gm::decodeBlock(im.p, im.readableBytes(), 0, N,
                                       5000, gm::kPb0, gm::kKb0, 16, logicalBytes);
    TASK_CHECK(parsed.status == 0 && parsed.consumed == cons,
               "runs(adaptive): model parse");
    for (uint32_t i = 0; i < N && i < parsed.out.size(); ++i)
        TASK_CHECK(parsed.out[i] == plan[i], "runs(adaptive): model sample");
}

static void runsClampCase() {
    // n = 70000 > N_MAX_MEAN_CLAMP (65535) forces mb = 65535; the next k is 7.
    std::vector<int32_t> plan = {35000, 4, -5, 2};
    gm::Block mdl = gm::encodePlan(plan, 10, 0, 14, 24);
    TASK_CHECK(mdl.mains[0].code.n == 70000, "runs(clamp): n over threshold");
    TASK_CHECK(mdl.mains[0].meanAfter == 65535, "runs(clamp): mb clamped");
    TASK_CHECK(mdl.mains[1].code.k == 7, "runs(clamp): next k=7");

    size_t logicalBytes = (mdl.encoded.size() + 64) / 8 + 16;
    gi::Image im(logicalBytes);
    auto pk = gm::packAt(mdl.encoded.b, 0, logicalBytes, gi::kStreamFill);
    std::memcpy(im.p, pk.data(), logicalBytes);
    std::vector<int32_t> out(plan.size() + 8, (int32_t)0x71717171);
    AGParamRec ap = makeAG(10, 0, 14, (uint32_t)plan.size());
    BitBuffer bb;
    BitBufferInit(&bb, im.p, (uint32_t)logicalBytes);
    uint32_t cons = 0;
    int st = dyn_decomp(&ap, &bb, out.data(), (int32_t)plan.size(), 24, &cons);
    TASK_CHECK(st == 0 && cons == mdl.consumed, "runs(clamp): decode");
    for (size_t i = 0; i < plan.size(); ++i)
        TASK_CHECK(out[i] == plan[i], "runs(clamp): sample");
}

static void runsOverrunCase() {
    // Finite stream: one sample then a modelled run of 20, but only 8 samples
    // are declared.  Production must return kALAC_ParamError and must not
    // write past the first residual (the zero fill is after the bounds check).
    std::vector<int32_t> plan = {3};
    for (int i = 0; i < 20; ++i) plan.push_back(0);

    gm::Block mdl = gm::encodePlan(plan, 10, 0, 14, 16);
    TASK_CHECK(mdl.runs.size() == 1 && mdl.runs[0].length == 20,
               "runs(overrun): modelled run=20");

    size_t logicalBytes = (mdl.encoded.size() + 32) / 8 + 16;
    gi::Image im(logicalBytes);
    auto pk = gm::packAt(mdl.encoded.b, 0, logicalBytes, gi::kStreamFill);
    std::memcpy(im.p, pk.data(), logicalBytes);

    const uint32_t declared = 8;
    std::vector<int32_t> out(declared + 8, (int32_t)0x77777777);
    AGParamRec ap = makeAG(10, 0, 14, declared);
    BitBuffer bb;
    BitBufferInit(&bb, im.p, (uint32_t)logicalBytes);
    uint32_t cons = 0;
    int st = dyn_decomp(&ap, &bb, out.data(), (int32_t)declared, 16, &cons);

    TASK_CHECK(out[0] == 3, "runs(overrun): prefix residual present");
    for (uint32_t i = 1; i < declared + 8; ++i)
        TASK_CHECK(out[i] == (int32_t)0x77777777,
                   "runs(overrun): output sentinel untouched");
    TASK_CHECK(st == kALAC_ParamError, "runs(overrun): original param error");
    TASK_CHECK(im.hardGuardsIntact(), "runs(overrun): buffer sentinels intact");

    gm::Block me = gm::decodeBlock(im.p, im.readableBytes(), 0, declared,
                                   10, 0, 14, 16, logicalBytes);
    TASK_CHECK(me.status == gm::STATUS_PARAM, "runs(overrun): model agrees error");
}

static int runsGroup() {
    // Small lengths first so a missing +zmode correction is the first
    // content difference; cover positive and negative follow values.
    runsCase(0, 5);
    runsCase(0, -6);
    runsCase(1, 7);
    runsCase(1, -8);
    const uint32_t lens[] = {2, 7, 40, 257, 4096, 65534, 65535};
    const int32_t pos[] = {9, 11, 13, 17, 19, 23, 29};
    const int32_t neg[] = {-10, -12, -14, -18, -20, -24, -31};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); ++i) {
        runsCase(lens[i], pos[i]);
        runsCase(lens[i], neg[i]);
    }

    runsAdaptiveDecayCase();
    runsClampCase();

    // Block independence: a dirty/clamped block followed by re-initialised
    // AG params must decode a canonical block exactly like a fresh params.
    {
        std::vector<int32_t> canon = {2, 0, 0, 5, -6, 0, 0, 0, 3, -4};
        gm::Block mdl = gm::encodePlan(canon, 10, 0, 14, 16);
        size_t lb = (mdl.encoded.size() + 32) / 8 + 16;
        gi::Image a(lb), b(lb);
        auto pk = gm::packAt(mdl.encoded.b, 0, lb, gi::kStreamFill);
        std::memcpy(a.p, pk.data(), lb);
        std::memcpy(b.p, pk.data(), lb);

        std::vector<int32_t> oa(canon.size() + 4, 0x11111111);
        std::vector<int32_t> ob(canon.size() + 4, 0x22222222);

        AGParamRec dirty = makeAG(65000, 40, 14, (uint32_t)canon.size());
        // touch it with a clamp-style decode first (same storage)
        {
            std::vector<int32_t> junk(4, 0);
            gi::Image jb(64);
            BitBuffer jbb; BitBufferInit(&jbb, jb.p, 64);
            uint32_t jc = 0;
            dyn_decomp(&dirty, &jbb, junk.data(), 1, 24, &jc);
        }
        // re-init from mb0 exactly as a new channel element does
        dirty = makeAG(10, 0, 14, (uint32_t)canon.size());
        BitBuffer ba; BitBufferInit(&ba, a.p, (uint32_t)lb);
        uint32_t ca = 0;
        int sa = dyn_decomp(&dirty, &ba, oa.data(), (int32_t)canon.size(), 16, &ca);

        AGParamRec fresh = makeAG(10, 0, 14, (uint32_t)canon.size());
        BitBuffer bb; BitBufferInit(&bb, b.p, (uint32_t)lb);
        uint32_t cb = 0;
        int sb2 = dyn_decomp(&fresh, &bb, ob.data(), (int32_t)canon.size(), 16, &cb);

        TASK_CHECK(sa == 0 && sb2 == 0, "runs(reset): status");
        TASK_CHECK(ca == cb && ca == mdl.consumed, "runs(reset): bit count");
        for (size_t i = 0; i < canon.size(); ++i) {
            TASK_CHECK(oa[i] == canon[i], "runs(reset): reinitialised block");
            TASK_CHECK(ob[i] == oa[i], "runs(reset): fresh vs reset identical");
        }
    }

    runsOverrunCase();
    return 1;
}

// ===========================================================================
// packets group
// ===========================================================================
static uint32_t le24(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t expect24(int32_t residual, int lowByte) {
    uint32_t u = (uint32_t)residual;
    if (lowByte >= 0) u = (u << 8) | (uint32_t)(lowByte & 0xff);
    return u & 0xffffffu;
}

static int packetsGroup() {
    // ---- P1: SCE 16-bit mono, exact channel ------------------------------
    {
        const uint32_t N = 16;
        std::vector<int32_t> res = {3, 0, 0, 0, 7, -8, 2, 0,
                                   0, -5, 1, 4, 0, 6, -7, 2};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        w.ag(res, 16);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 1, 128);
        ALACDecoder dec;
        TASK_CHECK(dec.Init(cookie.data(), (uint32_t)cookie.size()) == 0,
                   "packets: init SCE16");
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(N + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 1, &ns);
        TASK_CHECK(st == 0, "packets: SCE16 status");
        TASK_CHECK(ns == N, "packets: SCE16 frame count");
        for (uint32_t i = 0; i < N; ++i)
            TASK_CHECK(pcm[i] == (int16_t)res[i], "packets: SCE16 PCM");
        TASK_CHECK(im.hardGuardsIntact(), "packets: SCE16 guards");
    }

    // ---- P2: SCE 16 with run, numChannels=2 deficit so ID_END is read ----
    {
        const uint32_t N = 12;
        std::vector<int32_t> res = {3, 0, 0, 0, 7, -8, 2, 0, 0, -5, 1, 4};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        w.ag(res, 16);
        w.u(7, 3); w.byteAlign();

        std::vector<uint8_t> pkt = w.packed();
        TASK_CHECK(((pkt[2] >> 1) & 1) == 0, "packets: SCE compressed flag");
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 1, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(2 * N + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 2, &ns);
        TASK_CHECK(st == 0, "packets: SCE16 deficit/ID_END status");
        TASK_CHECK(ns == N, "packets: SCE16 deficit count");
        for (uint32_t i = 0; i < N; ++i)
            TASK_CHECK(pcm[2 * i] == (int16_t)res[i],
                       "packets: post-run nonzero reaches output");
        TASK_CHECK((uint32_t)BitBufferGetPosition(&bb) == (uint32_t)pkt.size() * 8u,
                   "packets: read ID_END and byte-aligned position");
    }

    // ---- P3: CPE 16-bit stereo with runs and +/- follow values ----------
    {
        const uint32_t N = 12;
        std::vector<int32_t> L = {2, 0, 0, 5, -3, 0, 7, -8, 1, 0, 0, 4};
        std::vector<int32_t> R = {-4, 1, 0, 0, 0, 9, -9, 2, 0, 6, -7, 3};
        gp::Writer w;
        w.tag(1, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        gp::predictorOrderZeroHeader(w);
        w.ag(L, 17);
        w.ag(R, 17);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        TASK_CHECK(((pkt[2] >> 1) & 1) == 0, "packets: CPE compressed flag");
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 2, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(2 * N + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 2, &ns);
        TASK_CHECK(st == 0, "packets: CPE16 status");
        TASK_CHECK(ns == N, "packets: CPE16 count");
        for (uint32_t i = 0; i < N; ++i) {
            TASK_CHECK(pcm[2 * i] == (int16_t)L[i], "packets: CPE16 L PCM");
            TASK_CHECK(pcm[2 * i + 1] == (int16_t)R[i], "packets: CPE16 R PCM");
        }
    }

    // ---- P4: SCE 24-bit no shift ----------------------------------------
    {
        const uint32_t N = 8;
        std::vector<int32_t> res = {100, -200, 300, -400,
                                   500000, -500001, 7, -8};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        w.ag(res, 24);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(24, 1, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<uint8_t> pcm(3 * N + 16, 0xcc);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, pcm.data(), N, 1, &ns);
        TASK_CHECK(st == 0, "packets: SCE24 status");
        TASK_CHECK(ns == N, "packets: SCE24 count");
        for (uint32_t i = 0; i < N; ++i)
            TASK_CHECK(le24(&pcm[3 * i]) == expect24(res[i], -1),
                       "packets: SCE24 PCM");
    }

    // ---- P5: CPE 24-bit no shift ----------------------------------------
    {
        const uint32_t N = 8;
        std::vector<int32_t> L = {10, -20, 0, 300, -4000, 5, -6, 7};
        std::vector<int32_t> R = {-11, 21, 301, 0, 4001, -5, 6, -7};
        gp::Writer w;
        w.tag(1, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        gp::predictorOrderZeroHeader(w);
        w.ag(L, 25);
        w.ag(R, 25);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(24, 2, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<uint8_t> pcm(6 * N + 32, 0xcc);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, pcm.data(), N, 2, &ns);
        TASK_CHECK(st == 0, "packets: CPE24 status");
        TASK_CHECK(ns == N, "packets: CPE24 count");
        for (uint32_t i = 0; i < N; ++i) {
            TASK_CHECK(le24(&pcm[6 * i]) == expect24(L[i], -1),
                       "packets: CPE24 L");
            TASK_CHECK(le24(&pcm[6 * i + 3]) == expect24(R[i], -1),
                       "packets: CPE24 R");
        }
    }

    // ---- P6: SCE 24-bit with one low byte shifted off --------------------
    {
        const uint32_t N = 8;
        std::vector<int32_t> res = {100, -200, 300, -400, 500, -600, 700, -800};
        std::vector<uint8_t> low = {0x11, 0x22, 0x33, 0x44,
                                    0x55, 0x66, 0x77, 0x88};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12);
        w.u((0 << 3) | (1 << 1) | 0, 4);   // bytesShifted = 1
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        for (uint32_t i = 0; i < N; ++i) w.u(low[i], 8); // shift region
        w.ag(res, 16);                                    // chanBits = 16
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(24, 1, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<uint8_t> pcm(3 * N + 16, 0xcc);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, pcm.data(), N, 1, &ns);
        TASK_CHECK(st == 0, "packets: SCE24-shift status");
        for (uint32_t i = 0; i < N; ++i)
            TASK_CHECK(le24(&pcm[3 * i]) == expect24(res[i], low[i]),
                       "packets: SCE24 low-byte shift-off PCM");
    }

    // ---- P7: CPE 24-bit with one shifted byte per channel ---------------
    {
        const uint32_t N = 6;
        std::vector<int32_t> L = {120, -240, 360, -480, 12, -34};
        std::vector<int32_t> R = {-130, 250, -370, 490, -56, 78};
        uint8_t lowU[6] = {1, 2, 3, 4, 5, 6};
        uint8_t lowV[6] = {9, 8, 7, 6, 5, 4};
        gp::Writer w;
        w.tag(1, 0); w.zeros(12);
        w.u((0 << 3) | (1 << 1) | 0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        gp::predictorOrderZeroHeader(w);
        for (uint32_t i = 0; i < N; ++i) {     // interleaved U,V low bytes
            w.u(lowU[i], 8);
            w.u(lowV[i], 8);
        }
        w.ag(L, 17);                            // 24-8+1
        w.ag(R, 17);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(24, 2, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<uint8_t> pcm(6 * N + 32, 0xcc);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, pcm.data(), N, 2, &ns);
        TASK_CHECK(st == 0, "packets: CPE24-shift status");
        for (uint32_t i = 0; i < N; ++i) {
            TASK_CHECK(le24(&pcm[6 * i]) == expect24(L[i], lowU[i]),
                       "packets: CPE24-shift L");
            TASK_CHECK(le24(&pcm[6 * i + 3]) == expect24(R[i], lowV[i]),
                       "packets: CPE24-shift R");
        }
    }

    // ---- P8: partial frame (CPE16), declared block 64, only P valid -----
    {
        const uint32_t BLOCK = 64, P = 13;
        std::vector<int32_t> L(P), R(P);
        for (uint32_t i = 0; i < P; ++i) {
            L[i] = (i == 4) ? 22 : ((int32_t)(i % 3) - 1);
            R[i] = (i == 8) ? -25 : (1 - (int32_t)(i % 4));
        }
        gp::Writer w;
        w.tag(1, 0); w.zeros(12);
        w.u((1u << 3), 4);                 // partialFrame = 1
        w.u(P, 32);                        // 32-bit partial length
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w);
        gp::predictorOrderZeroHeader(w);
        w.ag(L, 17);
        w.ag(R, 17);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 2, BLOCK);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(2 * BLOCK + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), BLOCK, 2, &ns);
        TASK_CHECK(st == 0, "packets: partial status");
        TASK_CHECK(ns == P, "packets: partial valid frame count");
        for (uint32_t i = 0; i < P; ++i) {
            TASK_CHECK(pcm[2 * i] == (int16_t)L[i], "packets: partial L");
            TASK_CHECK(pcm[2 * i + 1] == (int16_t)R[i], "packets: partial R");
        }
    }

    // ---- P9a: two adjacent SCE elements + ID_END, 3 requested channels --
    {
        const uint32_t N = 10;
        std::vector<int32_t> A = {1, -2, 3, 0, 0, 4, -5, 6, 0, 7};
        std::vector<int32_t> B = {-7, 6, -5, 0, 4, -3, 2, 0, -1, 8};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8); gp::predictorOrderZeroHeader(w); w.ag(A, 16);
        w.tag(0, 1); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8); gp::predictorOrderZeroHeader(w); w.ag(B, 16);
        w.u(7, 3); w.byteAlign();

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 3, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(3 * N + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 3, &ns);
        TASK_CHECK(st == 0, "packets: adjacent SCE+SCE status");
        for (uint32_t i = 0; i < N; ++i) {
            TASK_CHECK(pcm[3 * i] == (int16_t)A[i], "packets: adjacent SCE A");
            TASK_CHECK(pcm[3 * i + 1] == (int16_t)B[i], "packets: adjacent SCE B");
        }
        TASK_CHECK((uint32_t)BitBufferGetPosition(&bb) == (uint32_t)pkt.size() * 8u,
                   "packets: adjacent elements then ID_END position");
    }

    // ---- P9b: adjacent SCE + CPE in one packet (3 channels) -------------
    {
        const uint32_t N = 8;
        std::vector<int32_t> A = {1, 2, -3, 4, 0, 5, -6, 7};
        std::vector<int32_t> L = {10, 20, -30, 40, 0, 50, -60, 70};
        std::vector<int32_t> R = {-11, 21, 31, -41, 0, -51, 61, -71};
        gp::Writer w;
        w.tag(0, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8); gp::predictorOrderZeroHeader(w); w.ag(A, 16);
        w.tag(1, 0); w.zeros(12); w.u(0, 4);
        w.u(0, 8); w.u(0, 8);
        gp::predictorOrderZeroHeader(w); gp::predictorOrderZeroHeader(w);
        w.ag(L, 17); w.ag(R, 17);
        w.u(7, 3);

        std::vector<uint8_t> pkt = w.packed();
        gi::Image im(pkt.size());
        std::memcpy(im.p, pkt.data(), pkt.size());
        auto cookie = gp::makeCookie(16, 3, 128);
        ALACDecoder dec; dec.Init(cookie.data(), (uint32_t)cookie.size());
        BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
        std::vector<int16_t> pcm(3 * N + 8, (int16_t)0x9999);
        uint32_t ns = 0;
        int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 3, &ns);
        TASK_CHECK(st == 0, "packets: adjacent SCE+CPE status");
        for (uint32_t i = 0; i < N; ++i) {
            TASK_CHECK(pcm[3 * i] == (int16_t)A[i], "packets: SCE+CPE ch0");
            TASK_CHECK(pcm[3 * i + 1] == (int16_t)L[i], "packets: SCE+CPE ch1");
            TASK_CHECK(pcm[3 * i + 2] == (int16_t)R[i], "packets: SCE+CPE ch2");
        }
    }

    // ---- P10: consecutive packets of different lengths ------------------
    {
        const uint32_t lens[5] = {5, 11, 23, 37, 9};
        auto cookie = gp::makeCookie(16, 2, 64);
        ALACDecoder dec;
        TASK_CHECK(dec.Init(cookie.data(), (uint32_t)cookie.size()) == 0,
                   "packets: consecutive init");
        for (uint32_t q = 0; q < 5; ++q) {
            uint32_t N = lens[q];
            std::vector<int32_t> L(N), R(N);
            for (uint32_t i = 0; i < N; ++i) {
                L[i] = (int32_t)((i + q) % 5) - 2;
                R[i] = (i == N / 2) ? (int32_t)(13 + q)
                                    : (int32_t)((i * 3 + q) % 7) - 3;
            }
            gp::Writer w;
            w.tag(1, 0); w.zeros(12); w.u(0, 4);
            w.u(0, 8); w.u(0, 8);
            gp::predictorOrderZeroHeader(w);
            gp::predictorOrderZeroHeader(w);
            gm::Block bl = w.ag(L, 17);
            gm::Block br = w.ag(R, 17);
            w.u(7, 3);

            std::vector<uint8_t> pkt = w.packed();
            gi::Image im(pkt.size());
            std::memcpy(im.p, pkt.data(), pkt.size());
            BitBuffer bb; BitBufferInit(&bb, im.p, (uint32_t)pkt.size());
            std::vector<int16_t> pcm(2 * N + 8, (int16_t)0x9999);
            uint32_t ns = 0;
            int st = dec.Decode(&bb, (uint8_t*)pcm.data(), N, 2, &ns);
            TASK_CHECK(st == 0, "packets: consecutive status");
            TASK_CHECK(ns == N, "packets: consecutive count");
            for (uint32_t i = 0; i < N; ++i) {
                TASK_CHECK(pcm[2 * i] == (int16_t)L[i], "packets: consecutive L");
                TASK_CHECK(pcm[2 * i + 1] == (int16_t)R[i], "packets: consecutive R");
            }
            // CPE order-0 header is 71 bits; decoder stops at AG end.
            uint32_t expectPos = 71u + (uint32_t)bl.consumed + (uint32_t)br.consumed;
            TASK_CHECK((uint32_t)BitBufferGetPosition(&bb) == expectPos,
                       "packets: consecutive bit position");
        }
    }

    // ---- P11: frozen production-encoder compressed packets ---------------
    {
        TASK_CHECK(((ENC16_PACKET[2] >> 1) & 1) == 0,
                   "packets: frozen ENC16 is compressed, not escape");
        gi::Image im16(ENC16_PACKET_LEN);
        std::memcpy(im16.p, ENC16_PACKET, ENC16_PACKET_LEN);
        ALACDecoder dec16;
        TASK_CHECK(dec16.Init((void*)ENC16_COOKIE, ENC16_COOKIE_LEN) == 0,
                   "packets: frozen ENC16 init");
        BitBuffer b16; BitBufferInit(&b16, im16.p, ENC16_PACKET_LEN);
        std::vector<int16_t> p16(ENC_FRAMES * ENC_CHANNELS + 16,
                                 (int16_t)0x9999);
        uint32_t ns16 = 0;
        int s16 = dec16.Decode(&b16, (uint8_t*)p16.data(),
                               ENC_FRAMES, ENC_CHANNELS, &ns16);
        TASK_CHECK(s16 == 0, "packets: frozen ENC16 decode");
        TASK_CHECK(ns16 == ENC_FRAMES, "packets: frozen ENC16 frames");
        TASK_CHECK(std::memcmp(p16.data(), ENC16_PCM,
                               ENC_FRAMES * ENC_CHANNELS * 2u) == 0,
                   "packets: frozen ENC16 PCM compatibility");
        TASK_CHECK(im16.hardGuardsIntact(), "packets: frozen ENC16 guards");

        TASK_CHECK(((ENC24_PACKET[2] >> 1) & 1) == 0,
                   "packets: frozen ENC24 is compressed, not escape");
        gi::Image im24(ENC24_PACKET_LEN);
        std::memcpy(im24.p, ENC24_PACKET, ENC24_PACKET_LEN);
        ALACDecoder dec24;
        TASK_CHECK(dec24.Init((void*)ENC24_COOKIE, ENC24_COOKIE_LEN) == 0,
                   "packets: frozen ENC24 init");
        BitBuffer b24; BitBufferInit(&b24, im24.p, ENC24_PACKET_LEN);
        std::vector<uint8_t> p24(ENC_FRAMES * ENC_CHANNELS * 3 + 32, 0xcc);
        uint32_t ns24 = 0;
        int s24 = dec24.Decode(&b24, p24.data(), ENC_FRAMES, ENC_CHANNELS, &ns24);
        TASK_CHECK(s24 == 0, "packets: frozen ENC24 decode");
        TASK_CHECK(ns24 == ENC_FRAMES, "packets: frozen ENC24 frames");
        TASK_CHECK(std::memcmp(p24.data(), ENC24_PCM,
                               ENC_FRAMES * ENC_CHANNELS * 3u) == 0,
                   "packets: frozen ENC24 PCM compatibility");
    }

    return 1;
}

} // namespace

int task_group(const char* group) {
    if (group == 0) return 0;
    if (std::strcmp(group, "codes") == 0)   return codesGroup();
    if (std::strcmp(group, "runs") == 0)    return runsGroup();
    if (std::strcmp(group, "packets") == 0) return packetsGroup();
    return 0;
}
