/*
 * Content regression for the LC3 spectral entropy layer.
 *
 * Three groups are driven entirely from explicit, deterministic plans
 * and the independent C++ reference in entropy_reference.cpp:
 *
 *   planes    - forward interval coder vs backward ordinary bit plane,
 *               checked in both directions against the production plane
 *               reader/writer exposed by the C bridge.
 *   spectrum  - signed coefficient plans, escape/context/LSB/residual
 *               branches, cross-checked against the real static stages.
 *   frames    - full legal packets through lc3_decode, including a TNS
 *               filter sharing the arithmetic state, and a corrupt
 *               packet proven to enter PLC.
 *
 * Expectations are never produced by the functions under test.
 */

#include "checks.h"
#include "entropy_reference.h"
#include "target_bridge.h"

#include <lc3.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace entref;

namespace {

/* Branch coverage ledger: every required format branch is latched when
 * the independent reference actually executes it, and asserted later. */
struct Coverage {
    bool half_switch = false;
    bool high_rate_off = false;
    bool high_rate_on = false;
    bool lsb_off = false;
    bool lsb_on = false;
    bool two_escapes = false;
    bool residual_correction = false;
    bool lsb_unit_recover = false;
    bool truncate_mid_pair = false;
    bool truncate_before_sign = false;
    bool carry_delayed = false;
    bool pending_ff_run = false;
    bool shared_last_byte = false;
    bool accu32_cross = false;
};

Coverage g_cov;

[[noreturn]] void fail(const std::string& msg)
{
    std::fprintf(stderr, "LC3_ENTROPY_MISMATCH %s\n", msg.c_str());
    std::exit(2);
}

/* Infrastructure/setup problems are deliberately distinct from content
 * findings: they never use the LC3_ENTROPY_MISMATCH prefix and exit with
 * a non-2 status, so a harness cannot mistake them for a detected bug. */
[[noreturn]] void infra_fail(const std::string& msg)
{
    std::fprintf(stderr, "LC3_ENTROPY_INFRA_ERROR %s\n", msg.c_str());
    std::exit(3);
}

void check(bool cond, const std::string& msg)
{
    /* Every real comparison is counted through the shared harness
     * counter, and a content failure keeps the required prefix and
     * exit status while carrying the detailed diagnostic. */
    ++task_checks;
    if (!cond)
        fail(msg);
}

void require(bool cond, const std::string& msg)
{
    if (!cond)
        infra_fail(msg);
}

/* Deterministic pseudo-random generator for self-made vectors. */
struct Rng {
    unsigned s;
    explicit Rng(unsigned seed) : s(seed) {}
    unsigned next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
    }
    int between(int lo, int hi) {
        return lo + (int)(next() % (unsigned)(hi - lo + 1));
    }
};

std::string loc(int dt, int sr, int nbytes, int idx)
{
    char b[160];
    std::snprintf(b, sizeof(b), "dt=%d sr=%d nbytes=%d idx=%d",
                  dt, sr, nbytes, idx);
    return std::string(b);
}

/* ------------------------------------------------------------------ */
/* planes group                                                        */
/* ------------------------------------------------------------------ */

struct PlanePlan {
    std::vector<bridge_op_t> ops;
};

PlanePlan make_plane_plan(int model, unsigned seed, int extra_bits)
{
    PlanePlan pp;
    Rng rng(seed);
    /* A fixed long symbol run known to delay a carry across several
     * pending 0xff bytes, interleaved with ordinary bits. */
    const unsigned seq[] = {
        15,13,15,8,16,11,12,4,11,3,5,5,15,10,16,2,2,12,16,16,16,13,
        6,15,13,1,6,12,3,15,14,4,16,14,12,0,5,3,3,10,8,3,1,9,16,15,16
    };
    for (unsigned sym : seq)
        pp.ops.push_back({BRIDGE_OP_SYMBOL, model, sym, 0, 0});
    for (int i = 0; i < extra_bits; i++)
        pp.ops.push_back({BRIDGE_OP_BIT, 0, 0, 1, rng.next() & 1u});
    return pp;
}

void run_planes()
{
    const int nbytes = 96;
    PlanePlan pp = make_plane_plan(40, 777, 47);

    ByteStore store(nbytes);
    BackWriter bw(store);
    AcEncoder ac;
    ac.begin(store);
    for (auto& o : pp.ops) {
        if (o.kind == BRIDGE_OP_SYMBOL)
            ac.put_symbol(spectrum_model(o.model), o.sym);
        else
            bw.write_bit((int)o.value);
    }
    long ordinary_before_terminate = bw.pos;
    ac.terminate(bw);

    check(ac.carry_events >= 1, "planes: delayed carry not exercised");
    check(ac.pending_ff_peak >= 3, "planes: pending 0xff run not exercised");
    g_cov.carry_delayed = true;
    g_cov.pending_ff_run = true;

    int ff_run = 0, max_ff = 0;
    for (int i = 0; i < ac.fw; i++) {
        if (store.b[i] == 0xff) { ff_run++; max_ff = std::max(max_ff, ff_run); }
        else ff_run = 0;
    }
    check(max_ff >= 2, "planes: expected consecutive 0xff bytes");
    check((ordinary_before_terminate & 7) != 0,
          "planes: ordinary plane is byte aligned");

    bridge_plane_t prod_read;
    bridge_prod_read(store.b.data(), nbytes,
                     pp.ops.data(), (int)pp.ops.size(), &prod_read);
    check(prod_read.error == 0, "planes: production read flagged error");

    AcDecoder indep_dec(store.b.data(), nbytes);
    BackReader indep_bw(store.b.data(), nbytes, 0, 8L * nbytes);
    int op_idx = 0;
    for (auto& o : pp.ops) {
        if (o.kind == BRIDGE_OP_SYMBOL) {
            unsigned got = indep_dec.get_symbol_linear(spectrum_model(o.model));
            check((int)got == prod_read.symbols[op_idx],
                  "planes: symbol disagreement indep->prod");
            check(got == o.sym, "planes: symbol not recovered");
        } else {
            unsigned v;
            check(indep_bw.read(1, v), "planes: ordinary bit exhausted");
            check(v == prod_read.value[op_idx],
                  "planes: ordinary bit disagreement indep->prod");
            check(v == o.value, "planes: ordinary bit not recovered");
        }
        op_idx++;
    }

    int indep_left =
        indep_dec.logical_bits_left(nbytes, indep_bw.consumed());
    check(std::abs(indep_left - prod_read.bits_left) <= 1,
          "planes: logical remaining bit count mismatch");

    std::vector<uint8_t> prod_bytes(nbytes, 0);
    bridge_prod_write(nbytes, pp.ops.data(), (int)pp.ops.size(),
                      prod_bytes.data());
    int byte_diffs = 0;
    for (int i = 0; i < nbytes; i++)
        byte_diffs += prod_bytes[i] != store.b[i];
    check(byte_diffs == 0, "planes: writers emit different bytes");

    AcDecoder back_dec(prod_bytes.data(), nbytes);
    BackReader back_bw(prod_bytes.data(), nbytes, 0, 8L * nbytes);
    for (auto& o : pp.ops) {
        if (o.kind == BRIDGE_OP_SYMBOL) {
            unsigned got = back_dec.get_symbol_linear(spectrum_model(o.model));
            check(got == o.sym, "planes: prod->indep symbol mismatch");
        } else {
            unsigned v;
            check(back_bw.read(1, v), "planes: prod->indep bit exhausted");
            check(v == o.value, "planes: prod->indep bit mismatch");
        }
    }
    check(!back_dec.error, "planes: independent decode error");

    int back_bytes = (int)((bw.pos + 7) >> 3);
    /* The forward interval stream and the backward ordinary plane meet
     * with at most a single shared meeting byte (gap of 0 or 1 byte). */
    int gap_bytes = (nbytes - back_bytes) - ac.fw;
    check(gap_bytes == 0 || gap_bytes == 1,
          "planes: AC termination does not meet backward plane");
    check((ordinary_before_terminate & 7) != 0 && ac.fw < nbytes,
          "planes: last byte not shared by both planes");
    g_cov.shared_last_byte = true;

    /* Ordinary bits crossing the 32-bit accumulator (39 bits, including
     * a wide 25-bit op and a non-byte-aligned boundary). */
    std::vector<bridge_op_t> wide;
    std::vector<unsigned> wide_expect;
    wide.push_back({BRIDGE_OP_BITS, 0, 0, 25, 0x1234567u & ((1u<<25)-1)});
    wide_expect.push_back(0x1234567u & ((1u<<25)-1));
    wide.push_back({BRIDGE_OP_BIT, 0, 0, 1, 1});
    wide_expect.push_back(1);
    wide.push_back({BRIDGE_OP_BITS, 0, 0, 13, 0x1abcu});
    wide_expect.push_back(0x1abcu);
    std::vector<uint8_t> wb(nbytes, 0);
    bridge_prod_write(nbytes, wide.data(), (int)wide.size(), wb.data());
    bridge_plane_t wr;
    bridge_prod_read(wb.data(), nbytes, wide.data(), (int)wide.size(), &wr);
    check(wr.error == 0, "planes: wide-op read error");
    for (int i = 0; i < (int)wide.size(); i++)
        check(wr.value[i] == wide_expect[i],
              "planes: wide accumulator value mismatch");
    g_cov.accu32_cross = true;

    BackReader wide_bw(wb.data(), nbytes, 0, 8L * nbytes);
    for (auto& o : wide) {
        unsigned v;
        check(wide_bw.read(o.nbits, v), "planes: wide indep exhausted");
        check(v == o.value, "planes: wide indep value mismatch");
    }

    /* Tight-packet case: a deterministically chosen shape where the
     * backward ordinary plane and the interval coder terminate inside
     * the very same byte (gap == 0). */
    {
        const int tight_n = 32;
        std::vector<bridge_op_t> tight;
        for (int i = 0; i < 6; i++)
            tight.push_back({BRIDGE_OP_SYMBOL, 1,
                             (unsigned)((i * 7 + 3) % 17), 0, 0});
        for (int i = 0; i < 8; i++)
            tight.push_back({BRIDGE_OP_BIT, 0, 0, 1,
                             (unsigned)((i * 3 + 1) & 1)});
        std::vector<uint8_t> tb(tight_n, 0);
        bridge_prod_write(tight_n, tight.data(), (int)tight.size(),
                          tb.data());
        bridge_plane_t tr;
        bridge_prod_read(tb.data(), tight_n, tight.data(),
                         (int)tight.size(), &tr);
        check(tr.error == 0, "planes: tight read error");
        ByteStore tstore(tight_n);
        BackWriter tbw(tstore);
        AcEncoder tac; tac.begin(tstore);
        for (auto& o : tight) {
            if (o.kind == BRIDGE_OP_SYMBOL)
                tac.put_symbol(spectrum_model(o.model), o.sym);
            else
                tbw.write_bit((int)o.value);
        }
        tac.terminate(tbw);
        int t_back = (int)((tbw.pos + 7) >> 3);
        int t_gap = (tight_n - t_back) - tac.fw;
        check(t_gap == 0, "planes: planes do not share the last byte");
        g_cov.shared_last_byte = true;
        for (int i = 0; i < tight_n; i++)
            check(tb[i] == tstore.b[i], "planes: tight byte mismatch");
    }

    /* Prove every planes branch actually executed. */
    check(g_cov.carry_delayed && g_cov.pending_ff_run,
          "planes: delayed carry / pending 0xff branch not covered");
    check(g_cov.shared_last_byte, "planes: shared last byte not covered");
    check(g_cov.accu32_cross, "planes: 32-bit crossing not covered");
}

/* ------------------------------------------------------------------ */
/* spectrum group                                                      */
/* ------------------------------------------------------------------ */

/* Build a deterministic signed coefficient vector that exercises the
 * requested escape/LSB branches. Values are integer quantized levels. */
std::vector<int> make_coeffs(int nq, unsigned seed, int mag)
{
    std::vector<int> xq(nq, 0);
    Rng rng(seed);
    for (int i = 0; i < nq; i++) {
        int r = rng.between(0, 9);
        int v = 0;
        if (r < 3) v = rng.between(1, 3);
        else if (r < 7) v = rng.between(1, mag);
        else if (r < 9) v = rng.between(4, mag + 4);
        xq[i] = (rng.next() & 1u) ? v : -v;
    }
    return xq;
}

struct SpecCfg {
    int dt, sr, nbytes, nq;
    bool lsb_mode;
    int g_idx, nf;
    unsigned seed;
    int mag;
};

void run_one_spectrum(const SpecCfg& c)
{
    int ne = ne_of(c.dt, c.sr);
    check(c.nq <= ne && (c.nq & 1) == 0, "spectrum: bad nq plan");

    FramePlan fp;
    fp.dt = c.dt; fp.sr = c.sr; fp.nbytes = c.nbytes;
    fp.bw = c.sr;                       /* full bandwidth for the rate */
    fp.spec.dt = c.dt; fp.spec.sr = c.sr; fp.spec.nbytes = c.nbytes;
    fp.spec.bw = c.sr;
    fp.spec.nq = c.nq;
    fp.spec.lsb_mode = c.lsb_mode;
    fp.spec.g_idx = c.g_idx;
    fp.spec.nf = c.nf;
    fp.spec.xq = make_coeffs(c.nq, c.seed, c.mag);

    /* Independent plan description: escape layers, sign positions and
     * context evolution, before any packet is produced. */
    std::vector<PairTrace> desc = ref_describe_plan(fp.spec);
    check((int)desc.size() == c.nq / 2, "spectrum: pair count mismatch");
    int max_escape = 0;
    bool saw_half1 = false;
    for (auto& t : desc) {
        max_escape = std::max(max_escape, t.escape_layers);
        if (t.half == 1) saw_half1 = true;
    }
    if (c.nq > ((ne + 2) >> 1)) {
        check(saw_half1, "spectrum: second-half model not reached");
        g_cov.half_switch = true;
    }
    if (max_escape >= 2)
        g_cov.two_escapes = true;

    /* Each model index populated by the independent plan must equal the
     * format-table entry returned by the production data accessor. */
    for (auto& t : desc) {
        for (int k = 0; k < 4; k++) {
            if (t.model_base[k] == 0 && k > 0)
                continue;
            check(t.model_base[k] ==
                      (int)bridge_lut(
                          high_rate_mode(c.sr, c.nbytes) ? 1 : 0,
                          t.half, t.state_in, k),
                  "spectrum: context model index mismatch");
        }
    }

    /* Independent mode/gain formulas vs the production format functions. */
    int lsb_flag = 0;
    int hr = bridge_resolve_modes(c.sr, c.nbytes, &lsb_flag);
    check(hr == (int)high_rate_mode(c.sr, c.nbytes),
          "spectrum: high-rate formula divergence");
    check(lsb_flag == (int)lsb_allowed(c.sr, c.nbytes),
          "spectrum: LSB-allow formula divergence");
    check(bridge_gain_offset(c.sr, c.nbytes) ==
              gain_offset(c.sr, c.nbytes),
          "spectrum: gain offset formula divergence");
    check(std::fabs((double)bridge_unquantize_gain(17) -
                    unquant_gain(17)) < 1e-5,
          "spectrum: unquantized gain divergence");

    /* Residual (LSB off) or deferred LSB ordinary-bit plan, a short
     * deterministic sequence comfortably inside the packet budget. */
    Rng brng(c.seed ^ 0x55aa);
    if (!c.lsb_mode) {
        fp.spec.residual_bits.clear();
        for (int i = 0; i < 12; i++)
            fp.spec.residual_bits.push_back(brng.next() & 1u);
    } else {
        fp.spec.lsb_extra_bits.clear();
        for (int i = 0; i < 24; i++)
            fp.spec.lsb_extra_bits.push_back(brng.next() & 1u);
    }

    std::vector<uint8_t> packet = ref_build_frame(fp);

    /* Independent readback from the very first packet byte. */
    FrameReadback rb = ref_read_frame(fp, packet.data(), c.nbytes);
    check(rb.ok, "spectrum: independent readback failed");
    check(!rb.spec.stopped_early, "spectrum: unexpected early stop");

    /* Production static stages over the same bytes. */
    bridge_stages_t st;
    bridge_spec_stages(packet.data(), c.nbytes, c.dt, c.sr, c.sr,
                       c.nq, c.lsb_mode ? 1 : 0, c.g_idx, &st);
    check(st.ret == 0, std::string("spectrum: production stage error ") +
                       loc(c.dt, c.sr, c.nbytes, st.ret));
    check(st.error == 0, "spectrum: production bitstream error");
    check(st.nf == c.nf, "spectrum: noise factor not recovered");

    /* Integer quantized values must be exactly equal. */
    for (int i = 0; i < c.nq; i++) {
        if ((int)rb.spec.quant[i] != (int)std::lround(st.xq[i]))
            fail(std::string("spectrum: quant mismatch ") +
                 loc(c.dt, c.sr, c.nbytes, i));
    }
    /* Independent noise seed vs the production static stage seed. */
    check(rb.spec.nf_seed_quant == st.nf_seed_quant,
          "spectrum: quant noise seed mismatch");

    /* Refined (post residual/LSB) values, exact for integer LSB, tight
     * tolerance for the fractional residual correction. */
    bool refined_changed = false;
    for (int i = 0; i < c.nq; i++) {
        double diff = std::fabs(rb.spec.refined[i] - (double)st.xref[i]);
        double tol = c.lsb_mode ? 0.0 : 1e-6;
        if (diff > tol)
            fail(std::string("spectrum: refined mismatch ") +
                 loc(c.dt, c.sr, c.nbytes, i));
        if (rb.spec.refined[i] != (double)rb.spec.quant[i])
            refined_changed = true;
    }
    if (!c.lsb_mode && !fp.spec.residual_bits.empty()) {
        check(refined_changed, "spectrum: residual branch not exercised");
        g_cov.residual_correction = true;
    }

    /* Gained spectrum after gain and noise filling. */
    for (int i = 0; i < ne; i++) {
        double refv = rb.spec.gained[i];
        double prodv = (double)st.xfinal[i];
        double tol = 2e-5 * (1.0 + std::fabs(refv));
        if (std::fabs(refv - prodv) > tol)
            fail(std::string("spectrum: gained mismatch ") +
                 loc(c.dt, c.sr, c.nbytes, i));
    }
    check(rb.spec.nf_seed == st.nf_seed_final,
          "spectrum: final noise seed mismatch");

    /* Independent noise estimator vs the production static estimator.
     * Both see a zero-padded buffer of size ne (coefficients past nq
     * are zero), matching a clean decoder spectrum buffer. */
    std::vector<int> xq_only(ne, 0);
    for (int i = 0; i < c.nq; i++) xq_only[i] = rb.spec.quant[i];
    int nf_ref = ref_estimate_noise(c.dt, c.sr, xq_only, c.nq);
    std::vector<float> fx(ne + 8, 0.0f);
    for (int i = 0; i < c.nq; i++) fx[i] = (float)xq_only[i];
    int nf_prod = bridge_estimate_noise(c.dt, c.sr, fx.data(), c.nq);
    check(nf_ref == nf_prod, "spectrum: noise estimator divergence");

    if (c.lsb_mode) g_cov.lsb_on = true;
    else g_cov.lsb_off = true;
    if (high_rate_mode(c.sr, c.nbytes)) g_cov.high_rate_on = true;
    else g_cov.high_rate_off = true;
}

/* LSB-on case where a high-part-zero coefficient is recovered as a
 * signed unit value from the deferred low bits. The deferred bit vector
 * is an explicit deterministic plan; the index at which a zero high
 * part becomes -1 and +1 is established by the independent reader and
 * then confirmed against the production static stage. */
void run_lsb_unit_recovery()
{
    const int dt = 3, sr = 4, nbytes = 160, nq = 48;
    FramePlan fp;
    fp.dt = dt; fp.sr = sr; fp.nbytes = nbytes; fp.bw = sr;
    fp.spec.dt = dt; fp.spec.sr = sr; fp.spec.nbytes = nbytes;
    fp.spec.bw = sr; fp.spec.nq = nq; fp.spec.lsb_mode = true;
    fp.spec.g_idx = 40; fp.spec.nf = 3;
    fp.spec.xq.assign(nq, 0);
    /* Mix of zero and >=4 coefficients so several pairs carry LSB data,
     * including zero high parts eligible for unit insertion. */
    fp.spec.xq[0] = 0;  fp.spec.xq[1] = 6;
    fp.spec.xq[2] = -5; fp.spec.xq[3] = 0;
    for (int i = 4; i < nq; i++)
        fp.spec.xq[i] = (i % 5 == 0) ? (i % 2 ? -7 : 8) : (i % 3);

    /* Explicit deferred-LSB plan (prefix 01100000 then a fixed tail).
     * The independent decoder maps coefficient 0 to -1 and coefficient
     * 41 to +1; both have a zero quantized high part. */
    const int prefix[] = {0, 1, 1, 0, 0, 0, 0, 0};
    for (int b : prefix)
        fp.spec.lsb_extra_bits.push_back(b);
    for (int i = 0; i < 24; i++)
        fp.spec.lsb_extra_bits.push_back((i * 5 + 2) & 1);

    std::vector<uint8_t> packet = ref_build_frame(fp);
    FrameReadback rb = ref_read_frame(fp, packet.data(), nbytes);
    check(rb.ok, "lsb: independent readback failed");

    bridge_stages_t st;
    bridge_spec_stages(packet.data(), nbytes, dt, sr, sr,
                       nq, 1, fp.spec.g_idx, &st);
    check(st.ret == 0 && st.error == 0, "lsb: production stage error");

    const int neg_idx = 0, pos_idx = 41;
    check(rb.spec.quant[neg_idx] == 0 && rb.spec.quant[pos_idx] == 0,
          "lsb: unit recovery requires zero quantized high parts");
    check((int)rb.spec.refined[neg_idx] == -1,
          "lsb: negative unit coefficient not recovered");
    check((int)rb.spec.refined[pos_idx] == 1,
          "lsb: positive unit coefficient not recovered");
    check((int)std::lround(st.xref[neg_idx]) == -1,
          "lsb: production negative unit mismatch");
    check((int)std::lround(st.xref[pos_idx]) == 1,
          "lsb: production positive unit mismatch");
    for (int i = 0; i < nq; i++) {
        if (std::fabs(rb.spec.refined[i] - (double)st.xref[i]) > 1e-6)
            fail(std::string("lsb: refined mismatch ") +
                 loc(dt, sr, nbytes, i));
    }
    g_cov.lsb_unit_recover = true;
    g_cov.lsb_on = true;
}

/* Explicit deferred-bit exhaustion: the independent reader must stop at
 * a pair boundary / before a sign bit, never zero-filling the rest.
 * Cuts are placed inside the quantization region, not the residual
 * plane, so an actual symbol's ordinary bits are withheld. */
void run_truncation()
{
    const int dt = 3, sr = 4, nbytes = 120, nq = 64;
    FramePlan fp;
    fp.dt = dt; fp.sr = sr; fp.nbytes = nbytes; fp.bw = sr;
    fp.spec.dt = dt; fp.spec.sr = sr; fp.spec.nbytes = nbytes;
    fp.spec.bw = sr; fp.spec.nq = nq; fp.spec.lsb_mode = false;
    fp.spec.g_idx = 30; fp.spec.nf = 4;
    fp.spec.xq = make_coeffs(nq, 909, 18);
    for (int i = 0; i < 16; i++)
        fp.spec.residual_bits.push_back(i & 1);

    std::vector<uint8_t> packet = ref_build_frame(fp);
    FrameReadback full = ref_read_frame(fp, packet.data(), nbytes);
    check(full.ok, "trunc: full readback failed");

    /* Absolute ordinary-bit position where quantization ends. */
    long quant_ord = 0;
    for (auto& p : full.spec.pairs)
        quant_ord += p.sign_bits + (long)p.lsb_bits.size();
    long quant_end = full.header_ordinary + quant_ord;

    bool saw_mid_pair = false, saw_before_sign = false;
    /* Several caps strictly inside the quantization region. */
    for (long depth = 8; depth < quant_ord - 8; depth += 17) {
        long cap = full.header_ordinary + depth;
        FrameReadback cut =
            ref_read_frame_budget(fp, packet.data(), nbytes, cap);
        check(cut.spec.stopped_early,
              "trunc: reader did not stop on exhausted budget");
        check(!cut.ok, "trunc: exhausted read reported ok");
        check(cut.spec.stop_pair >= 0, "trunc: no stop pair recorded");
        check((int)cut.spec.pairs.size() <=
                  (int)full.spec.pairs.size(),
              "trunc: decoded more pairs than the full stream");
        if (!cut.spec.pairs.empty() &&
            cut.spec.pairs.back().stop_at_sign)
            saw_before_sign = true;
        else
            saw_mid_pair = true;
    }
    check(quant_end < full.ordinary_consumed,
          "trunc: residual plane not present after quantization");
    check(saw_mid_pair, "trunc: no mid-pair exhaustion observed");
    check(saw_before_sign, "trunc: no before-sign exhaustion observed");
    g_cov.truncate_mid_pair = saw_mid_pair;
    g_cov.truncate_before_sign = saw_before_sign;
}

void run_spectrum()
{
    /* All primary plans are deliberately under-subscribed: the packet
     * budget comfortably holds the spectrum plus the residual plane, so
     * the two planes never collide. Exhaustion is covered separately by
     * run_truncation(). nq/mag are conservative fixed plans, not derived
     * from the functions under test. */

    /* high-rate OFF at 48 kHz, large magnitudes force >=2 escape
     * layers even on a short coded span. */
    run_one_spectrum({3, 4, 80,  48,  false, 30, 4, 12, 30});
    /* high-rate ON at 48 kHz, wider span. */
    run_one_spectrum({3, 4, 120, 110, false, 35, 3, 202, 10});
    /* 7.5 ms at 8 kHz, smallest packet, low-rate context. */
    run_one_spectrum({2, 0, 24,  12,  false, 20, 5, 303, 4});
    /* 7.5 ms at 16 kHz, nq past the half-spectrum boundary so the second
     * half context model is entered; high-rate ON and 2 escape layers. */
    run_one_spectrum({2, 1, 60,  84,  false, 25, 4, 404, 8});
    /* 10 ms at 24 kHz, high-rate ON, second half and 3 escape layers. */
    run_one_spectrum({2, 2, 70, 100,  false, 28, 4, 505, 10});
    /* 10 ms at 32 kHz, nbytes exactly on the high-rate threshold (80 is
     * not greater than 20*4), exercising the OFF side at this rate. */
    run_one_spectrum({3, 3, 80,  96,  false, 33, 3, 606, 8});
    /* LSB allowed and active. */
    run_one_spectrum({3, 4, 160, 120, true,  40, 3, 707, 12});

    run_lsb_unit_recovery();
    run_truncation();

    check(g_cov.half_switch, "coverage: half-spectrum switch missing");
    check(g_cov.high_rate_off && g_cov.high_rate_on,
          "coverage: high-rate threshold sides missing");
    check(g_cov.two_escapes, "coverage: two escape layers missing");
    check(g_cov.residual_correction, "coverage: residual correction missing");
    check(g_cov.lsb_unit_recover, "coverage: LSB unit recovery missing");
    check(g_cov.lsb_off && g_cov.lsb_on, "coverage: LSB modes missing");
    check(g_cov.truncate_mid_pair && g_cov.truncate_before_sign,
          "coverage: truncation positions missing");
}

/* ------------------------------------------------------------------ */
/* frames group                                                        */
/* ------------------------------------------------------------------ */

/* Build a legal full-frame plan with a deterministic spectrum. */
FramePlan make_frame_plan(int dt, int sr, int nbytes, int nq,
                          bool tns_on, unsigned seed, int mag)
{
    FramePlan fp;
    fp.dt = dt; fp.sr = sr; fp.nbytes = nbytes; fp.bw = sr;
    fp.spec.dt = dt; fp.spec.sr = sr; fp.spec.nbytes = nbytes;
    fp.spec.bw = sr;
    fp.spec.nq = nq;
    fp.spec.lsb_mode = lsb_allowed(sr, nbytes);
    fp.spec.g_idx = 36;
    fp.spec.nf = 3;
    fp.spec.xq = make_coeffs(nq, seed, mag);
    fp.pitch_present = false;
    fp.sns_lfcb = fp.sns_hfcb = 0;

    if (tns_on) {
        fp.tns.enabled = true;
        fp.tns.lpc_weighting = (nbytes * 8) < (120 * (1 + dt));
        fp.tns.order = 4;
        fp.tns.rc[0] = 2; fp.tns.rc[1] = -3;
        fp.tns.rc[2] = 1; fp.tns.rc[3] = -2;
    }

    Rng brng(seed ^ 0x9e37);
    int nplane = fp.spec.lsb_mode ? 24 : 12;
    for (int i = 0; i < nplane; i++) {
        int b = brng.next() & 1u;
        if (fp.spec.lsb_mode)
            fp.spec.lsb_extra_bits.push_back(b);
        else
            fp.spec.residual_bits.push_back(b);
    }
    return fp;
}

const int FRAME_DT_US = 10000;
const int FRAME_SR_HZ = 48000;

/* Decode one good frame and cross-check the traced pre-TNS spectrum
 * against the independent reference. */
void check_good_frame(const FramePlan& fp, lc3_decoder_t dec,
                      std::vector<float>& pcm, int stride)
{
    int ne = ne_of(fp.dt, fp.sr);
    std::vector<uint8_t> packet = ref_build_frame(fp);

    /* Independent reader advances from packet start through side info
     * and the TNS arithmetic symbols before reaching the spectrum. */
    FrameReadback rb = ref_read_frame(fp, packet.data(), fp.nbytes);
    check(rb.ok, "frames: independent frame readback failed");
    if (fp.tns.enabled)
        check(rb.tns_symbols == 1 + fp.tns.order,
              "frames: TNS symbols not shared with the AC state");

    /* The trace hook is compiled into the production bridge under
     * LC3_ENTROPY_TRACE; the fixed build always enables it. The snapshot
     * is taken after spectral decode and before TNS synthesis. */
    bridge_trace_reset();
    int ret = lc3_decode(dec, packet.data(), fp.nbytes,
                         LC3_PCM_FORMAT_FLOAT, pcm.data(), stride);
    check(ret == 0, "frames: good frame did not decode cleanly");

    std::vector<float> traced(ne, 0.0f);
    int got = bridge_trace_copy(traced.data(), ne);
    check(got == ne, "frames: trace captured wrong coefficient count");
    check(bridge_trace_frames() == 1, "frames: trace observation missing");
    for (int i = 0; i < ne; i++) {
        double refv = rb.spec.gained[i];
        double prodv = traced[i];
        double tol = 2e-5 * (1.0 + std::fabs(refv));
        if (std::fabs(refv - prodv) > tol)
            fail(std::string("frames: traced spectrum mismatch ") +
                 loc(fp.dt, fp.sr, fp.nbytes, i));
    }
}

void run_frames()
{
    const int dt = 3, sr = 4, nbytes = 120, nq = 120;
    int ns = lc3_frame_samples(FRAME_DT_US, FRAME_SR_HZ);
    int ne = ne_of(dt, sr);
    check(ns == 480 && ne == 400, "frames: unexpected frame dimensions");

    /* ---- A: TNS enabled, its AC symbols share the spectrum state. ---- */
    FramePlan fp_tns = make_frame_plan(dt, sr, nbytes, nq, true, 1111, 16);
    require(fp_tns.tns.enabled, "frames: TNS plan not enabled");

    /* ---- B: no TNS, same spectrum shape for history comparison. ---- */
    FramePlan fp_plain = make_frame_plan(dt, sr, nbytes, nq, false, 1111, 16);

    std::vector<float> pcm_a(ns, 0.0f);

    /* Decoder A: a run of good frames, first with TNS, then plain. */
    void* mem_a = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    lc3_decoder_t dec_a =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_a);
    require(dec_a != nullptr, "frames: decoder A setup failed");

    check_good_frame(fp_tns, dec_a, pcm_a, 1);

    /* Continuous nonzero PCM and output count. */
    int nonzero = 0, run = 0, best_run = 0;
    for (int i = 0; i < ns; i++) {
        if (pcm_a[i] != 0.0f) { nonzero++; run++; best_run = std::max(best_run, run); }
        else run = 0;
        check(std::isfinite(pcm_a[i]), "frames: non-finite PCM");
    }
    check(nonzero > ns / 2, "frames: too few nonzero PCM samples");
    check(best_run >= 16, "frames: no continuous nonzero PCM run");

    /* stride-2 guard region: odd slots must stay untouched. */
    std::vector<float> guard(2 * ns, 0.0f);
    for (int i = 0; i < ns; i++) guard[2 * i + 1] = 12345.0f;
    int ret = lc3_decode(dec_a, ref_build_frame(fp_plain).data(), nbytes,
                         LC3_PCM_FORMAT_FLOAT, guard.data(), 2);
    check(ret == 0, "frames: stride-2 decode failed");
    for (int i = 0; i < ns; i++) {
        check(guard[2 * i + 1] == 12345.0f,
              "frames: stride guard region overwritten");
        check(std::isfinite(guard[2 * i]), "frames: bad stride PCM");
    }

    /* ---- Two-instance interleave. Four decoders share identical
     * history; each decodes the next frame exactly once so the ring
     * state is not advanced twice. ---- */
    void* mem_b = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    void* mem_a2 = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    void* mem_b2 = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    lc3_decoder_t dec_b =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_b);
    lc3_decoder_t dec_a2 =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_a2);
    lc3_decoder_t dec_b2 =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_b2);
    require(dec_b && dec_a2 && dec_b2, "frames: interleave setup failed");

    /* Bring all four decoders through the identical history. */
    std::vector<float> pa(ns), pb(ns);
    for (int f = 0; f < 3; f++) {
        FramePlan fp = make_frame_plan(dt, sr, nbytes, nq,
                                       f == 0, 2000 + f, 12 + f);
        std::vector<uint8_t> p = ref_build_frame(fp);
        lc3_decode(dec_a,  p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pa.data(), 1);
        lc3_decode(dec_b,  p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pb.data(), 1);
        lc3_decode(dec_a2, p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pa.data(), 1);
        lc3_decode(dec_b2, p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pb.data(), 1);
    }

    FramePlan fp_next = make_frame_plan(dt, sr, nbytes, nq, false, 3333, 14);
    std::vector<uint8_t> next_packet = ref_build_frame(fp_next);
    std::vector<float> inter(2 * ns, 0.0f);
    lc3_decode(dec_a, next_packet.data(), nbytes,
               LC3_PCM_FORMAT_FLOAT, inter.data(), 2);
    lc3_decode(dec_b, next_packet.data(), nbytes,
               LC3_PCM_FORMAT_FLOAT, inter.data() + 1, 2);
    lc3_decode(dec_a2, next_packet.data(), nbytes,
               LC3_PCM_FORMAT_FLOAT, pa.data(), 1);
    lc3_decode(dec_b2, next_packet.data(), nbytes,
               LC3_PCM_FORMAT_FLOAT, pb.data(), 1);
    for (int i = 0; i < ns; i++) {
        check(inter[2 * i] == pa[i], "frames: interleave A slot mismatch");
        check(inter[2 * i + 1] == pb[i], "frames: interleave B slot mismatch");
    }

    /* ---- Corrupt frame that explicitly exhausts the bit budget. ----
     * Claim every coefficient pair in a packet far too small to hold
     * them, so the static decoder runs out of bits and the public entry
     * falls into PLC. This is a constructed exhaustion, not a bit flip. */
    FramePlan fp_bad = make_frame_plan(dt, sr, 40, ne, false, 4444, 30);
    fp_bad.spec.residual_bits.clear();
    std::vector<uint8_t> bad_packet = ref_build_frame(fp_bad);

    /* Prove through the static-stage bridge that rejection is caused by
     * an explicit bit-budget exhaustion (no bits left, bitstream error),
     * rather than an arbitrary single-bit corruption. */
    bridge_stages_t bad_st;
    bridge_spec_stages(bad_packet.data(), 40, dt, sr, sr,
                       ne, 0, fp_bad.spec.g_idx, &bad_st);
    check(bad_st.error != 0 || bad_st.bits_left_after_quant <= 0,
          "frames: corrupt frame does not exhaust the bit budget");

    /* Two fresh decoders with identical history. */
    void* mem_c = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    void* mem_d = calloc(1, lc3_decoder_size(FRAME_DT_US, FRAME_SR_HZ));
    lc3_decoder_t dec_c =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_c);
    lc3_decoder_t dec_d =
        lc3_setup_decoder(FRAME_DT_US, FRAME_SR_HZ, 0, mem_d);
    for (int f = 0; f < 3; f++) {
        FramePlan fp = make_frame_plan(dt, sr, nbytes, nq, false, 5500 + f, 12);
        std::vector<uint8_t> p = ref_build_frame(fp);
        lc3_decode(dec_c, p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pa.data(), 1);
        lc3_decode(dec_d, p.data(), nbytes, LC3_PCM_FORMAT_FLOAT, pb.data(), 1);
    }

    std::vector<float> pc_corrupt(ns), pc_null(ns);
    int r_corrupt = lc3_decode(dec_c, bad_packet.data(), 40,
                               LC3_PCM_FORMAT_FLOAT, pc_corrupt.data(), 1);
    int r_null = lc3_decode(dec_d, nullptr, 40,
                            LC3_PCM_FORMAT_FLOAT, pc_null.data(), 1);
    check(r_corrupt == 1, "frames: corrupt frame did not enter PLC");
    check(r_null == 1, "frames: explicit NULL did not report PLC");
    for (int i = 0; i < ns; i++)
        check(pc_corrupt[i] == pc_null[i],
              "frames: corrupt-frame PLC differs from explicit NULL");

    /* Following normal frame must also match, proving PLC state parity. */
    std::vector<uint8_t> good_packet = ref_build_frame(fp_next);
    std::vector<float> pc_after_c(ns), pc_after_d(ns);
    int r1 = lc3_decode(dec_c, good_packet.data(), nbytes,
                        LC3_PCM_FORMAT_FLOAT, pc_after_c.data(), 1);
    int r2 = lc3_decode(dec_d, good_packet.data(), nbytes,
                        LC3_PCM_FORMAT_FLOAT, pc_after_d.data(), 1);
    check(r1 == 0 && r2 == 0, "frames: post-PLC frame not clean");
    for (int i = 0; i < ns; i++)
        check(pc_after_c[i] == pc_after_d[i],
              "frames: post-PLC PCM diverges");

    free(mem_a); free(mem_b); free(mem_a2); free(mem_b2);
    free(mem_c); free(mem_d);
}

} /* namespace */

void task_run(const char* group)
{
    std::string g = group;
    if (g == "planes")
        run_planes();
    else if (g == "spectrum")
        run_spectrum();
    else if (g == "frames")
        run_frames();
    else
        fail(std::string("unknown group ") + group);
}
