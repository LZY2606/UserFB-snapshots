/*
 * Independent entropy reference implementation.
 *
 * Everything here is written from the LC3 packet format description:
 * a raw byte array, a backward LSB-first ordinary-bit cursor, a forward
 * 24-bit interval coder, linear interval search on decode, and the
 * spectral pair/context/escape/LSB/residual/noise/gain procedures.
 *
 * The production code in bits.c/spec.c is never called from this file,
 * and its accumulator cache and binary symbol search are not reused.
 * The probability distributions and spectral context constants live in
 * tables.c and are reached only through the target_bridge.c accessors.
 */

#include "entropy_reference.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace entref {

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int v) { return v < 0 ? -v : v; }

int ne_of(int dt, int sr)
{
    /* lc3_ne_2m5[sr] * (1 + dt), restricted to the ordinary LC3 rates. */
    static const int ne_2m5[5] = { 20, 40, 60, 80, 100 };
    return ne_2m5[sr] * (1 + dt);
}

int nbits_nq_of(int dt, int sr)
{
    int ne = ne_of(dt, sr);
    return 4 + (ne > 32) + (ne > 64) + (ne > 128) +
           (ne > 256) + (ne > 512);
}

int gain_offset(int sr, int nbytes)
{
    int sr_ind = sr;                 /* ordinary LC3: no HR indices */
    int g_off = (nbytes * 8) / (10 * (1 + sr_ind));
    return imin(255, 105 + 5 * (1 + sr_ind) + imin(g_off, 115));
}

bool high_rate_mode(int sr, int nbytes)
{
    return nbytes > 20 * (1 + sr);
}

bool lsb_allowed(int sr, int nbytes)
{
    return nbytes >= 20 * (3 + sr);
}

double unquant_gain(int g_int)
{
    return std::pow(10.0, (double)g_int / 28.0);
}

/* Context lookup: the 4 model indices for a (rate, half, state) context. */
static inline int lut_index(int high_rate, int half, int state, int k)
{
    return (int)bridge_lut(high_rate, half, state, k);
}

/* ------------------------------------------------------------------ */
/* Backward ordinary-bit plane                                         */
/* ------------------------------------------------------------------ */

void BackWriter::write_bit(int bit)
{
    long j = pos++;
    int idx = store.size() - 1 - (int)(j >> 3);
    if (bit)
        store.b[idx] |= (uint8_t)(1u << (j & 7));
    /* bytes start zeroed, so a 0 needs no store. */
}

void BackWriter::write(unsigned value, int n)
{
    for (int k = 0; k < n; k++)
        write_bit((value >> k) & 1);
}

BackReader::BackReader(const uint8_t* data, int len,
                       long start_pos, long bit_limit)
    : p(data), nbytes(len), pos(start_pos), start(start_pos),
      limit(bit_limit) {}

bool BackReader::read_bit(int& out)
{
    if (pos >= limit)
        return false;
    long j = pos++;
    int idx = nbytes - 1 - (int)(j >> 3);
    if (idx < 0)
        return false;
    out = (p[idx] >> (j & 7)) & 1;
    return true;
}

bool BackReader::read(int n, unsigned& out)
{
    unsigned v = 0;
    for (int k = 0; k < n; k++) {
        int bit;
        if (!read_bit(bit))
            return false;
        v |= (unsigned)bit << k;
    }
    out = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* Forward 24-bit interval encoder                                     */
/* ------------------------------------------------------------------ */

int AcEncoder::range_bits() const
{
    int n = 0;
    for (uint32_t r = range; r; r >>= 1)
        n++;
    return n;
}

int AcEncoder::pending_bits() const
{
    return 26 - range_bits() + ((cache >= 0) + carry_count) * 8;
}

void AcEncoder::shift_byte()
{
    if (low < 0xff0000u || carry) {
        if (cache >= 0 && fw < store->size())
            store->b[fw++] = (uint8_t)(cache + carry);
        for (int c = carry_count; c > 0; c--) {
            if (fw < store->size())
                store->b[fw++] = (uint8_t)(carry ? 0x00 : 0xff);
            carry_count--;
            carry_events++;
        }
        cache = (int)(low >> 16);
        carry = 0;
    } else {
        carry_count++;
        pending_ff_peak = imax((long)carry_count, pending_ff_peak);
    }
    low = (low << 8) & 0xffffffu;
    shift_events++;
}

void AcEncoder::write_renorm()
{
    while (range < 0x10000u) {
        range <<= 8;
        shift_byte();
    }
}

void AcEncoder::put_symbol(const AcModelTable* model, unsigned sym)
{
    const AcModelPod* s = model->s;
    uint32_t r = range >> 10;

    low += r * s[sym].low;
    range = r * s[sym].range;

    carry |= (int)(low >> 24);
    low &= 0xffffffu;

    if (range < 0x10000u)
        write_renorm();
}

void AcEncoder::terminate(BackWriter& bw)
{
    /* Independent port of the interval termination procedure. */
    int nbits = 25 - range_bits();
    uint32_t mask = 0xffffffu >> nbits;
    uint32_t val = low + mask;
    uint32_t high = low + range;

    bool over_val = (val >> 24) != 0;
    bool over_high = (high >> 24) != 0;

    val = (val & 0xffffffu) & ~mask;
    high &= 0xffffffu;

    if (over_val == over_high) {
        if (val + mask >= high) {
            nbits++;
            mask >>= 1;
            val = ((low + mask) & 0xffffffu) & ~mask;
        }
        carry |= (val < low);
    }

    low = val;

    for ( ; nbits > 8; nbits -= 8)
        shift_byte();
    shift_byte();

    int end_val = cache >> (8 - nbits);

    if (carry_count) {
        if (fw < store->size())
            store->b[fw++] = (uint8_t)cache;
        for ( ; carry_count > 1; carry_count--)
            if (fw < store->size())
                store->b[fw++] = 0xff;
        end_val = nbits < 8 ? 0 : 0xff;
    }

    /* Fill the gap with ordinary zero bits, so the two planes meet.
     * The backward cursor occupies ceil(pos/8) bytes from the end. */
    int partial = (int)(bw.pos & 7);
    int back_bytes = (int)((bw.pos + 7) >> 3);
    int nleft = (store->size() - back_bytes) - fw;
    int gap = 8 * nleft - partial;
    for (int g = 0; g < gap; g++)
        bw.write_bit(0);

    if (fw < store->size()) {
        store->b[fw] &= (uint8_t)(0xff >> nbits);
        store->b[fw] |= (uint8_t)(end_val << (8 - nbits));
    }
}

/* ------------------------------------------------------------------ */
/* Forward 24-bit interval decoder, linear interval search             */
/* ------------------------------------------------------------------ */

AcDecoder::AcDecoder(const uint8_t* data, int len)
    : p(data), nbytes(len)
{
    low = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

void AcDecoder::read_renorm()
{
    while (range < 0x10000u) {
        range <<= 8;
        uint8_t next = fw < nbytes ? p[fw++] : 0;
        low = ((low << 8) | next) & 0xffffffu;
    }
}

int AcDecoder::range_bits() const
{
    int n = 0;
    for (uint32_t r = range; r; r >>= 1)
        n++;
    return n;
}

int AcDecoder::logical_bits_left(int nbytes, long back_consumed) const
{
    /* Specification-level geometry:
     *   forward AC consumption = 8*(fw-3) + (26 - range_bits)
     *   backward consumption  = back_consumed ordinary bits
     * The remaining count is the packet size minus both planes. */
    long nbits_ac = 8L * (fw - 3) + (26 - range_bits());
    return (int)(8L * nbytes - back_consumed - nbits_ac);
}

unsigned AcDecoder::get_symbol_linear(const AcModelTable* model)
{
    const AcModelPod* s = model->s;
    uint32_t r = (range >> 10) & 0xffffu;

    if (low >= (r << 10))
        error = true;

    /* Explicit linear search through the cumulative-frequency intervals,
     * instead of the production binary decision tree. */
    int sym = 16;
    searches++;
    while (sym > 0 && low < r * s[sym].low)
        sym--;

    low -= r * s[sym].low;
    range = r * s[sym].range;

    if (range < 0x10000u)
        read_renorm();

    return (unsigned)sym;
}

/* ------------------------------------------------------------------ */
/* Spectral context and pair coding                                    */
/* ------------------------------------------------------------------ */

void ref_encode_spectrum(const SpecPlan& plan,
                         AcEncoder& ac, BackWriter& bw)
{
    int ne = ne_of(plan.dt, plan.sr);
    int high_rate = high_rate_mode(plan.sr, plan.nbytes);
    uint8_t state = 0;

    int pair = 0;
    for (int half = 0; half < 2; half++) {
        int bound = imin(plan.nq, half ? ne : (ne + 2) >> 1);
        for (int i = pair * 2; i < bound; i += 2, pair++) {
            uint32_t a = (uint32_t)iabs(plan.xq[i + 0]);
            uint32_t b = (uint32_t)iabs(plan.xq[i + 1]);

            uint32_t m = (a | b) >> 2;
            unsigned k = 0;

            if (m) {
                if (plan.lsb_mode) {
                    int idx = lut_index(high_rate, half, state, k++);
                    ac.put_symbol(spectrum_model(idx), 16);
                }
                for (m >>= plan.lsb_mode; m; m >>= 1, k++) {
                    bw.write_bit((a >> k) & 1);
                    bw.write_bit((b >> k) & 1);
                    int idx = lut_index(high_rate, half, state,
                                        imin((int)k, 3));
                    ac.put_symbol(spectrum_model(idx), 16);
                }
                a >>= plan.lsb_mode;
                b >>= plan.lsb_mode;
                unsigned shr = k - plan.lsb_mode;
                k = imin((int)k, 3);
                a >>= shr;
                b >>= shr;
            }

            if (a)
                bw.write_bit(plan.xq[i + 0] < 0);
            if (b)
                bw.write_bit(plan.xq[i + 1] < 0);

            int idx = lut_index(high_rate, half, state, k);
            ac.put_symbol(spectrum_model(idx), a + 4 * b);

            state = (state << 4) +
                (k > 1 ? 12 + k : 1 + (a + b) * (k + 1));
        }
    }

    /* Residual (LSB off) or deferred LSB plane. */
    if (!plan.lsb_mode) {
        for (int bit : plan.residual_bits)
            bw.write_bit(bit);
    } else {
        for (int bit : plan.lsb_extra_bits)
            bw.write_bit(bit);
    }
}

/* Decode one pair, recording every symbol and ordinary bit consumed.
 * Returns false only when the ordinary budget is exhausted. */
static bool decode_pair(AcDecoder& ac, BackReader& bw,
                        int high_rate, int half, uint8_t& state,
                        int lsb_mode, int max_shl,
                        int i, PairTrace& tr, int& u_out, int& v_out)
{
    tr.state_in = state;
    tr.half = half;
    tr.pair = i / 2;

    int u = 0, v = 0, k = 0, shl = 0;
    int idx0 = lut_index(high_rate, half, state, 0);
    tr.model_base[0] = idx0;
    unsigned s = ac.get_symbol_linear(spectrum_model(idx0));
    tr.symbols.push_back(s);

    if (lsb_mode && s >= 16) {
        k = 1;
        int idx = lut_index(high_rate, half, state, k);
        tr.model_base[1] = idx;
        s = ac.get_symbol_linear(spectrum_model(idx));
        tr.symbols.push_back(s);
        shl++;
    }

    while (s >= 16 && shl < max_shl) {
        int bu, bv;
        if (!bw.read_bit(bu) || !bw.read_bit(bv)) {
            tr.escape_layers = (int)tr.symbols.size();
            return false;      /* stopped in the middle of a pair */
        }
        tr.lsb_bits.push_back(bu);
        tr.lsb_bits.push_back(bv);
        u |= bu << shl;
        v |= bv << shl;

        if (k < 3)
            k++;
        int idx = lut_index(high_rate, half, state, k);
        tr.model_base[k] = idx;
        s = ac.get_symbol_linear(spectrum_model(idx));
        tr.symbols.push_back(s);
        shl++;
    }

    if (s >= 16) {
        tr.escape_layers = (int)tr.symbols.size();
        tr.q0 = tr.q1 = -1000000;
        return false;          /* out-of-range value, explicit failure */
    }

    tr.escape_layers = (int)tr.symbols.size() - 1;

    int a = s % 4;
    int b = s / 4;
    u |= a << shl;
    v |= b << shl;

    int su = 0, sv = 0;
    if (u) {
        if (!bw.read_bit(su)) {
            tr.stop_at_sign = true;   /* stopped before a sign bit */
            return false;
        }
    }
    if (v) {
        if (!bw.read_bit(sv)) {
            tr.stop_at_sign = true;   /* stopped before a sign bit */
            return false;
        }
    }
    tr.sign_bits = (u != 0) + (v != 0);
    u_out = su ? -u : u;
    v_out = sv ? -v : v;
    tr.q0 = u_out;
    tr.q1 = v_out;

    state = (state << 4) +
        (k > 1 ? 12 + k : 1 + (a + b) * (k + 1));
    return true;
}

/* Independent, plan-only description of the on-disk shape of every pair:
 * escape layers, sign positions and context evolution. No production
 * state is consulted; the integer coefficient plan is the sole input. */
std::vector<PairTrace> ref_describe_plan(const SpecPlan& plan)
{
    int ne = ne_of(plan.dt, plan.sr);
    int high_rate = high_rate_mode(plan.sr, plan.nbytes);
    std::vector<PairTrace> traces;
    uint8_t state = 0;
    int pair = 0;

    for (int half = 0; half < 2; half++) {
        int bound = imin(plan.nq, half ? ne : (ne + 2) >> 1);
        for (int i = pair * 2; i < bound; i += 2, pair++) {
            PairTrace tr;
            tr.pair = pair;
            tr.half = half;
            tr.state_in = state;
            tr.q0 = plan.xq[i];
            tr.q1 = plan.xq[i + 1];

            uint32_t a = (uint32_t)iabs(plan.xq[i]);
            uint32_t b = (uint32_t)iabs(plan.xq[i + 1]);

            uint32_t m = (a | b) >> 2;
            unsigned k = 0;
            if (m) {
                if (plan.lsb_mode) {
                    tr.model_base[k] =
                        lut_index(high_rate, half, state, k);
                    tr.symbols.push_back(16);
                    k++;
                }
                for (m >>= plan.lsb_mode; m; m >>= 1, k++) {
                    int kk = imin((int)k, 3);
                    tr.model_base[kk] =
                        lut_index(high_rate, half, state, kk);
                    tr.lsb_bits.push_back((a >> k) & 1);
                    tr.lsb_bits.push_back((b >> k) & 1);
                    tr.symbols.push_back(16);
                }
                a >>= plan.lsb_mode;
                b >>= plan.lsb_mode;
                unsigned shr = k - plan.lsb_mode;
                k = imin((int)k, 3);
                a >>= shr;
                b >>= shr;
            }
            tr.model_base[k] = lut_index(high_rate, half, state, k);
            tr.escape_layers = (int)tr.symbols.size();
            tr.sign_bits = (a != 0) + (b != 0);
            tr.symbols.push_back(a + 4 * b);
            traces.push_back(tr);

            state = (state << 4) +
                (k > 1 ? 12 + k : 1 + (a + b) * (k + 1));
        }
    }
    return traces;
}

int ref_estimate_noise(int dt, int bw,
                       const std::vector<int>& xq, int nq)
{
    int ne = ne_of(dt, imin(bw, 4));
    int w = 1 + (dt >= 2) + (dt >= 3);
    /* Coefficients past nq are zero in the production decode buffer. */
    auto at = [&](int idx) -> int {
        return (idx >= 0 && idx < nq && idx < (int)xq.size())
                   ? iabs(xq[idx]) : 0;
    };
    double sum = 0;
    int ns = 0, z = 0, i;

    for (i = 6 * (1 + dt) - w; i < imin(nq, ne); i++) {
        z = at(i) < 1 ? z + 1 : 0;
        if (z > 2 * w)
            sum += (double)at(i - w), ns++;
    }
    for ( ; i < ne + w; i++)
        if (++z > 2 * w)
            sum += (double)at(i - w), ns++;

    int nf = ns ? 8 - (int)((16.0 * sum) / ns + 0.5) : 8;
    return imin(imax(nf, 0), 7);
}

static void ref_fill_noise(int dt, int bw, int nf, uint16_t seed,
                           double g, std::vector<double>& x, int nq)
{
    int ne = ne_of(dt, imin(bw, 4));
    int w = 1 + (dt >= 2) + (dt >= 3);
    double s = g * (double)(8 - nf) / 16.0;
    int z = 0, i;

    for (i = 6 * (1 + dt) - w; i < imin(nq, ne); i++) {
        z = x[i] != 0.0 ? 0 : z + 1;
        if (z > 2 * w) {
            seed = (uint16_t)((13849 + seed * 31821) & 0xffff);
            x[i - w] = (seed & 0x8000) ? -s : s;
        }
    }
    for ( ; i < ne + w; i++)
        if (++z > 2 * w) {
            seed = (uint16_t)((13849 + seed * 31821) & 0xffff);
            x[i - w] = (seed & 0x8000) ? -s : s;
        }
}

/* Core spectrum decode on shared, already-positioned coder/readers. */
static SpecResult decode_core(const SpecPlan& plan,
                              AcDecoder& ac, BackReader& bw)
{
    SpecResult res;
    int ne = ne_of(plan.dt, plan.sr);
    int high_rate = high_rate_mode(plan.sr, plan.nbytes);
    int max_shl = 14;                       /* ordinary LC3 */

    res.quant.assign(ne, 0);
    res.refined.assign(ne, 0.0);
    res.gained.assign(ne, 0.0);

    uint16_t nf_seed = 0;
    uint8_t state = 0;
    int pair = 0;
    bool stopped = false;

    for (int half = 0; half < 2 && !stopped; half++) {
        int bound = imin(plan.nq, half ? ne : (ne + 2) >> 1);
        for (int i = pair * 2; i < bound; i += 2, pair++) {
            PairTrace tr;
            int u, v;
            if (!decode_pair(ac, bw, high_rate, half, state,
                             plan.lsb_mode, max_shl, i, tr, u, v)) {
                res.pairs.push_back(tr);
                stopped = true;
                res.stopped_early = true;
                res.stop_pair = pair;
                res.stop_reason =
                    tr.symbols.empty() ? "before first symbol" :
                    tr.q0 == -1000000    ? "escape stack exhausted" :
                    "ordinary bit budget exhausted";
                break;
            }
            res.pairs.push_back(tr);
            res.quant[i + 0] = u;
            res.quant[i + 1] = v;
            nf_seed = (uint16_t)((nf_seed +
                (iabs(u) & 0x7fff) * i +
                (iabs(v) & 0x7fff) * (i + 1)) & 0xffff);
            res.total_sign_bits += tr.sign_bits;
        }
    }

    res.total_ordinary_bits = bw.consumed();
    res.nf_seed = nf_seed;
    res.nf_seed_quant = nf_seed;

    if (stopped) {
        /* Explicit stop: never invent zero bits for unread symbols. */
        res.ok = false;
        return res;
    }

    for (int i = 0; i < plan.nq; i++)
        res.refined[i] = (double)res.quant[i];

    /* The residual/LSB plane runs from the backward cursor up to the
     * logical boundary of the forward AC coder. Compute that boundary
     * from the reference's own cursor geometry (never from production),
     * so the gap-zero bytes written at termination are read as ordinary
     * residual bits exactly as the specification reader consumes them. */
    long logical_left = ac.logical_bits_left(plan.nbytes, bw.consumed());
    long refine_limit = bw.pos + (logical_left > 0 ? logical_left : 0);
    if (refine_limit > bw.limit)
        refine_limit = bw.limit;
    bw.limit = refine_limit;

    if (!plan.lsb_mode) {
        /* Residual correction, driven solely by stream bits. */
        for (int i = 0; i < plan.nq && bw.bits_left() > 0; i++) {
            if (res.quant[i] == 0)
                continue;
            int bit;
            if (!bw.read_bit(bit))
                break;
            if (bit == 0)
                res.refined[i] -= res.quant[i] < 0 ? 5.0 / 16.0
                                                   : 3.0 / 16.0;
            else
                res.refined[i] += res.quant[i] > 0 ? 5.0 / 16.0
                                                   : 3.0 / 16.0;
        }
    } else {
        for (int i = 0; i < plan.nq && bw.bits_left() > 0; i += 2) {
            double a = std::fabs(res.refined[i]);
            double b = std::fabs(res.refined[i + 1]);
            if (std::fmax(a, b) < 4.0)
                continue;

            int bit;
            if (bw.bits_left() > 0 && bw.read_bit(bit) && bit) {
                if (a != 0.0) {
                    res.refined[i] += res.refined[i] < 0 ? -1 : 1;
                    nf_seed = (uint16_t)((nf_seed + i) & 0xffff);
                } else if (bw.bits_left() > 0 && bw.read_bit(bit)) {
                    res.refined[i] = bit ? -1 : 1;
                    nf_seed = (uint16_t)((nf_seed + i) & 0xffff);
                }
            }
            if (bw.bits_left() > 0 && bw.read_bit(bit) && bit) {
                if (b != 0.0) {
                    res.refined[i + 1] += res.refined[i + 1] < 0 ? -1 : 1;
                    nf_seed = (uint16_t)((nf_seed + i + 1) & 0xffff);
                } else if (bw.bits_left() > 0 && bw.read_bit(bit)) {
                    res.refined[i + 1] = bit ? -1 : 1;
                    nf_seed = (uint16_t)((nf_seed + i + 1) & 0xffff);
                }
            }
        }
        res.nf_seed = nf_seed;
    }

    int g_int = plan.g_idx - gain_offset(plan.sr, plan.nbytes);
    double g = unquant_gain(g_int);
    for (int i = 0; i < plan.nq; i++)
        res.gained[i] = res.refined[i] * g;

    /* Noise filling gate, identical to the production condition. */
    bool do_fill = plan.nq > 2 || res.quant[0] || res.quant[1] ||
                   plan.g_idx > 0 || plan.nf < 7;
    if (do_fill)
        ref_fill_noise(plan.dt, plan.bw, plan.nf, nf_seed,
                       g, res.gained, plan.nq);

    res.total_ordinary_bits = bw.consumed();
    res.ok = true;
    return res;
}

SpecResult ref_decode_spectrum(const SpecPlan& plan,
                               AcDecoder& ac, BackReader& bw)
{
    return decode_core(plan, ac, bw);
}

/* ------------------------------------------------------------------ */
/* Full frame construction and readback                                */
/* ------------------------------------------------------------------ */

static int bw_nbits(int sr)
{
    return (sr > 0) + (sr > 1) + (sr > 3);
}

static int tns_nfilters(int dt, int bw)
{
    return 1 + (dt >= 1 && bw >= 3);
}

std::vector<uint8_t> ref_build_frame(const FramePlan& plan)
{
    int n = plan.nbytes;
    ByteStore store(n);
    BackWriter bw(store);
    AcEncoder ac;
    ac.begin(store);

    /* Bandwidth indication. */
    bw.write((unsigned)plan.bw, bw_nbits(plan.sr));

    /* Spectral side data: coded count, LSB mode, gain index. */
    int nbits_nq = nbits_nq_of(plan.dt, plan.sr);
    bw.write((unsigned)(imax(plan.spec.nq >> 1, 1) - 1), nbits_nq);
    bw.write(plan.spec.lsb_mode ? 1u : 0u, 1);
    bw.write((unsigned)plan.spec.g_idx, 8);

    /* TNS: one plain enable bit per filter, then arithmetic symbols. */
    int nfilters = tns_nfilters(plan.dt, plan.bw);
    for (int f = 0; f < nfilters; f++) {
        bool on = plan.tns.enabled && f == 0;
        bw.write(on ? 1u : 0u, 1);
        if (!on)
            continue;
        ac.put_symbol(tns_order_model(plan.tns.lpc_weighting),
                      (unsigned)(plan.tns.order - 1));
        for (int k = 0; k < plan.tns.order; k++)
            ac.put_symbol(tns_coeff_model(k),
                          (unsigned)(8 + plan.tns.rc[k]));
    }

    /* Pitch present flag and the 38 SNS bits, an explicit all-zero plan.
     * The production SNS reader resolves mux_code 0 as shape 1, idx_a 0,
     * i.e. a bounded, legal pulse configuration (single pulse of 10). */
    bw.write(plan.pitch_present ? 1u : 0u, 1);
    bw.write((unsigned)plan.sns_lfcb, 5);
    bw.write((unsigned)plan.sns_hfcb, 5);
    /* SNS is 38 backward bits: shape MSB (1), gain (1), ls_a (1) and
     * the 25-bit mux field, following the two 5-bit codebooks. */
    bw.write(0u, 1 + 1 + 1 + 25);

    /* Noise factor and spectral entropy section. */
    bw.write((unsigned)plan.spec.nf, 3);
    ref_encode_spectrum(plan.spec, ac, bw);

    ac.terminate(bw);
    return store.b;
}

FrameReadback ref_read_frame(const FramePlan& plan,
                             const uint8_t* data, int nbytes)
{
    return ref_read_frame_budget(plan, data, nbytes, 8L * nbytes);
}

FrameReadback ref_read_frame_budget(const FramePlan& plan,
                                    const uint8_t* data, int nbytes,
                                    long ordinary_cap)
{
    FrameReadback out;
    BackReader bw(data, nbytes, 0, ordinary_cap);

    unsigned v;
    bw.read(bw_nbits(plan.sr), v);                  /* bandwidth */

    int nbits_nq = nbits_nq_of(plan.dt, plan.sr);
    bw.read(nbits_nq, v);
    bw.read(1, v);                                  /* lsb mode */
    bw.read(8, v);                                  /* gain index */

    AcDecoder ac(data, nbytes);
    int nfilters = tns_nfilters(plan.dt, plan.bw);
    for (int f = 0; f < nfilters; f++) {
        bw.read(1, v);                              /* enable */
        if (!v)
            continue;
        ac.get_symbol_linear(
            tns_order_model(plan.tns.lpc_weighting));
        for (int k = 0; k < plan.tns.order; k++)
            ac.get_symbol_linear(tns_coeff_model(k));
        out.tns_symbols += 1 + plan.tns.order;
    }

    bw.read(1, v);                                  /* pitch */
    bw.read(38, v);                                 /* SNS block */
    bw.read(3, v);                                 /* noise factor */

    out.header_ordinary = bw.pos;
    out.spec = decode_core(plan.spec, ac, bw);
    out.ordinary_consumed = bw.pos;
    out.ok = out.spec.ok;
    return out;
}

} /* namespace entref */
