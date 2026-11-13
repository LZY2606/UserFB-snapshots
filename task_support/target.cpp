/*
 * Content regression driver for the LC3 spectral entropy work.
 *
 * Three groups are driven by task_support/target.sh:
 *
 *   planes    Cross checks the standalone interval coder and backward bit
 *             cursor against the production bitstream layer, in both
 *             directions, including carries, pending 0xff bytes, the
 *             shared arithmetic termination byte and unaligned caches.
 *
 *   spectrum  Decodes real spectral streams from signed coefficient plans,
 *             comparing integer quantized values, residual/LSB values,
 *             noise seed, noise filling and gain scaled spectra against
 *             the production static stages.
 *
 *   frames    Connects the independent model to the public lc3_decode()
 *             entry point, including a non-zero TNS arithmetic prefix,
 *             the decode trace observation point, stride protection and
 *             an explicitly exhausted frame confirmed to enter PLC.
 *
 * Expected values always come from the explicit plan and the independent
 * reference, never from the function under test.
 */

#include "checks.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

extern "C" {
#include <lc3.h>
#include "bits.h"
#include "tables.h"
#include "spec.h"
#include "tns.h"
#include "sns.h"
#include "bwdet.h"
}

/* ------------------------------------------------------------------ */
/* Independent reference interface                                    */
/* ------------------------------------------------------------------ */

struct er_bits_handle;
struct er_ac_dec_handle;
struct er_ac_enc_handle;
struct er_spec_handle;
struct er_write_handle;
struct er_frame_handle;

struct er_pair_trace;
struct er_spec_result;
struct er_write_plan;
struct er_frame_side;
struct er_frame_result;
struct er_tns_plan;

extern "C" {

er_bits_handle *er_bits_open_read(const uint8_t *, int);
er_bits_handle *er_bits_open_write(uint8_t *, int);
void er_bits_close(er_bits_handle *);
int er_get_bit(er_bits_handle *);
unsigned er_get_bits(er_bits_handle *, int);
void er_put_bit(er_bits_handle *, int);
void er_put_bits(er_bits_handle *, unsigned, int);
int er_bits_used(const er_bits_handle *);
int er_bits_plain_left(const er_bits_handle *);

er_ac_dec_handle *er_ac_dec_open(const uint8_t *, int);
void er_ac_dec_close(er_ac_dec_handle *);
int er_ac_dec_symbol(er_ac_dec_handle *, const lc3_ac_model *);
int er_ac_dec_pos(const er_ac_dec_handle *);
uint32_t er_ac_dec_range(const er_ac_dec_handle *);
int er_ac_dec_error(const er_ac_dec_handle *);
int er_ac_dec_bits_used(const er_ac_dec_handle *);

er_ac_enc_handle *er_ac_enc_open(void);
void er_ac_enc_close(er_ac_enc_handle *);
void er_ac_enc_symbol(er_ac_enc_handle *, const lc3_ac_model *, int);
int er_ac_enc_terminate(er_ac_enc_handle *, uint8_t *, int, int);
int er_ac_enc_symbols(const er_ac_enc_handle *);
int er_ac_enc_max_pending(const er_ac_enc_handle *);
int er_ac_enc_carry_events(const er_ac_enc_handle *);
int er_ac_enc_pending(const er_ac_enc_handle *);
int er_ac_enc_low_ff(const er_ac_enc_handle *);
int er_ac_enc_bits_used(const er_ac_enc_handle *);

er_spec_handle *er_spec_open(void);
void er_spec_close(er_spec_handle *);
const er_spec_result *er_spec_get(const er_spec_handle *);
int er_spec_decode(er_ac_dec_handle *, er_bits_handle *, int, int, int,
    int, int, int, int, er_spec_handle *, char *, int);

er_write_handle *er_write_open(void);
void er_write_close(er_write_handle *);
er_write_plan *er_write_get_plan(er_write_handle *);
int er_spec_write(er_write_handle *, er_ac_enc_handle *,
    er_bits_handle *, int, int);
int er_residual_write(er_write_handle *, er_bits_handle *, int);
int er_lsb_write(er_write_handle *, er_bits_handle *, int);
int er_residual_demand(er_write_handle *);
int er_lsb_demand(er_write_handle *);

int er_lsb_allowed(int, int);
int er_gain_offset(int, int);
int er_nbits_nq(int);

er_frame_handle *er_frame_open(void);
void er_frame_close(er_frame_handle *);
const er_frame_result *er_frame_get(const er_frame_handle *);
int er_frame_parse(er_ac_dec_handle *, er_bits_handle *,
    const er_frame_side *, er_frame_handle *, char *, int);

} /* extern C */

/* Reference result layouts, mirrored from entropy_reference.cpp. */
#define ER_MAX_NE 480

struct er_pair_trace {
    int index, half;
    int state_before, state_after;
    int escapes;
    int models[16];
    int symbols[16];
    int nsym;
    int sign_pos[2];
    int esc_bits;
    int defer_bits;
    int u, v;
    int su, sv;
};

struct er_spec_result {
    int ne, nq;
    int lsb_mode, high_rate;
    int nf, g_idx;
    uint16_t nf_seed;
    int xq[ER_MAX_NE];
    float xr[ER_MAX_NE];
    float xg[ER_MAX_NE];
    int npairs;
    er_pair_trace pairs[ER_MAX_NE / 2];
    int max_escape;
    int second_half_pairs;
    int residual_applied;
    int lsb_unit_pos, lsb_unit_neg;
    int lsb_pairs;
    int budget_stop;
    int stop_pair;
    int stop_phase;
    int bits_left_at_entry;
    int bits_left_at_end;
};

struct er_write_plan {
    int ne, nq;
    int lsb_mode, high_rate;
    int xq[ER_MAX_NE];
    float xg[ER_MAX_NE];
};

struct er_tns_plan {
    int nfilters;
    int lpc_weighting;
    int order[2];
    int rc[2][8];
};

struct er_frame_side {
    int dt, sr, nbytes;
    int bw;
    int nq, lsb_mode, g_idx, nf;
    er_tns_plan tns;
    int pitch_present;
};

struct er_frame_result {
    er_frame_side side;
    er_spec_result spec;
    int ac_after_tns_pos;
    int plain_after_sns;
    int error;
};

/* ------------------------------------------------------------------ */
/* Production static stage bridges                                     */
/* ------------------------------------------------------------------ */

extern "C" {

int bridge_resolve_modes(int, int, int *, int *);
int bridge_gain_offset(int, int);
int bridge_get_side(lc3_bits_t *, int, int, lc3_spec_side_t *);
int bridge_get_quantized(lc3_bits_t *, int, int, int, int, int,
    float *, uint16_t *);
void bridge_get_residual(lc3_bits_t *, int, float *, int);
void bridge_get_lsb(lc3_bits_t *, int, float *, int, uint16_t *);
float bridge_unquantize_gain(int);
int bridge_estimate_noise(int, int, const float *, int);
void bridge_fill_noise(int, int, int, uint16_t, float, float *, int);
int bridge_spec_decode(lc3_bits_t *, int, int, int, int,
    const lc3_spec_side_t *, float *);
int bridge_unquantize(int, int, int, float *, int);

void bridge_put_quantized(lc3_bits_t *, int, int, int,
    const float *, int, int);
void bridge_put_residual(lc3_bits_t *, int, float *, int);
void bridge_put_lsb(lc3_bits_t *, int, const float *, int);
void bridge_spec_put_side(lc3_bits_t *, int, int,
    const lc3_spec_side_t *);
int bridge_get_bits_left(const lc3_bits_t *);
int bridge_check_bits(const lc3_bits_t *);

void bridge_trace_reset(void);
int bridge_trace_count(void);
int bridge_trace_copy(int, float *, int, int *, int *, int *);

} /* extern C */

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static const int kRates[5] = { 8000, 16000, 24000, 32000, 48000 };

static int sr_of_hz(int hz)
{
    for (int i = 0; i < 5; i++)
        if (kRates[i] == hz)
            return i;
    return -1;
}

static int dt_of_us(int us)
{
    return us == 7500 ? LC3_DT_7M5 : LC3_DT_10M;
}

static void mismatch(const char *fmt, ...)
{
    std::fprintf(stderr, "LC3_ENTROPY_MISMATCH ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::exit(2);
}

/* Deterministic LCG so every plan and bit pattern is reproducible. */
static uint32_t lcg_state = 0x12345678u;
static uint32_t lcg_next(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

/* ------------------------------------------------------------------ */
/* planes group                                                        */
/* ------------------------------------------------------------------ */

struct plane_event {
    int kind;            /* 0: symbol, 1: bit */
    int model;           /* spectrum model index, for symbols */
    int value;
};

/* Build one deterministic candidate event stream for a search seed. */
static std::vector<plane_event> build_plane_candidate(int seed, int &plain_bits)
{
    std::vector<plane_event> ev;
    plain_bits = 0;
    uint32_t local = 0x12345678u + (uint32_t)seed * 2654435761u;

    for (int round = 0; round < 6; round++) {
        for (int k = 0; k < 8; k++) {
            local = local * 1664525u + 1013904223u;
            int model = (local >> 9) % 64;
            local = local * 1664525u + 1013904223u;
            int sym = (local >> 13) % 17;
            ev.push_back({0, model, sym});
            if ((round + k + seed) % 3 == 0) {
                int w = 5 + ((round + k) % 11);   /* 5..15 bit runs */
                for (int b = 0; b < w; b++) {
                    local = local * 1664525u + 1013904223u;
                    ev.push_back({1, 0, (int)(local & 1)});
                    plain_bits++;
                }
            }
        }
    }
    /* A final long unaligned plain run, deliberately over 32 bits. */
    for (int b = 0; b < 41; b++) {
        local = local * 1664525u + 1013904223u;
        ev.push_back({1, 0, (int)(local & 1)});
        plain_bits++;
    }
    return ev;
}

/* Stress plan: deterministically search the candidate family for a stream
 * that really raises a carry and holds at least two consecutive pending
 * 0xff bytes, while still fitting the packet. */
static std::vector<plane_event> build_plane_plan(int nbytes, int &plain_bits)
{
    for (int seed = 0; seed < 4096; seed++) {
        int pb = 0;
        std::vector<plane_event> ev = build_plane_candidate(seed, pb);

        std::vector<uint8_t> scratch(nbytes, 0);
        er_ac_enc_handle *ac = er_ac_enc_open();
        er_bits_handle *h = er_bits_open_write(scratch.data(), nbytes);
        for (const plane_event &e : ev) {
            if (e.kind == 0)
                er_ac_enc_symbol(ac, lc3_spectrum_models + e.model, e.value);
            else
                er_put_bit(h, e.value);
        }
        int ce = er_ac_enc_carry_events(ac);
        int mp = er_ac_enc_max_pending(ac);
        int ab = er_ac_enc_terminate(ac, scratch.data(), nbytes, pb);
        er_ac_enc_close(ac);
        er_bits_close(h);

        if (ce > 0 && mp >= 2 && ab > 0) {
            plain_bits = pb;
            return ev;
        }
    }
    mismatch("no plane candidate exercised carry with pending 0xff bytes");
    return {};
}

static void replay_production_read(const uint8_t *buf, int nbytes,
    const std::vector<plane_event> &ev, std::vector<int> &got_sym,
    std::vector<int> &got_bit, int &left_at_end)
{
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_READ, (void *)buf, nbytes);

    for (const plane_event &e : ev) {
        if (e.kind == 0) {
            int s = (int)lc3_get_symbol(&bits,
                lc3_spectrum_models + e.model);
            got_sym.push_back(s);
        } else {
            got_bit.push_back(lc3_get_bit(&bits));
        }
    }
    left_at_end = lc3_get_bits_left(&bits);
    TASK_CHECK(lc3_check_bits(&bits) == 0);
}

static void standalone_to_production(int nbytes)
{
    int plain_bits = 0;
    std::vector<plane_event> ev = build_plane_plan(nbytes, plain_bits);

    std::vector<uint8_t> buf(nbytes, 0);
    er_ac_enc_handle *ac = er_ac_enc_open();
    er_bits_handle *pb = er_bits_open_write(buf.data(), nbytes);

    int n_sym = 0, n_bit = 0;
    for (const plane_event &e : ev) {
        if (e.kind == 0) {
            er_ac_enc_symbol(ac, lc3_spectrum_models + e.model, e.value);
            n_sym++;
        } else {
            er_put_bit(pb, e.value);
            n_bit++;
        }
    }

    /* Stress branches must really be exercised. */
    TASK_CHECK(er_ac_enc_carry_events(ac) > 0);
    TASK_CHECK(er_ac_enc_max_pending(ac) >= 2);

    int ac_bytes = er_ac_enc_terminate(ac, buf.data(), nbytes, plain_bits);
    TASK_CHECK(ac_bytes > 0);
    int plain_bytes = (plain_bits + 7) >> 3;
    TASK_CHECK(ac_bytes + plain_bytes <= nbytes);

    std::vector<int> got_sym, got_bit;
    int left_prod = 0;
    replay_production_read(buf.data(), nbytes, ev, got_sym, got_bit, left_prod);

    int si = 0, bi = 0;
    for (const plane_event &e : ev) {
        if (e.kind == 0)
            TASK_CHECK(got_sym[si++] == e.value);
        else
            TASK_CHECK(got_bit[bi++] == e.value);
    }
    TASK_CHECK(si == n_sym);
    TASK_CHECK(bi == n_bit);

    /* Logical remaining bits follow the format-level consumption formula
     * (the same one the bitstream specification uses): total plane minus
     * fractional forward interval bits minus backward consumed bits.  It
     * is recomputed from the reader positions rather than trusted, and is
     * an upper bound on the production count, which additionally reserves
     * its 4 byte accumulator prefetch. */
    er_ac_dec_handle *dac = er_ac_dec_open(buf.data(), nbytes);
    er_bits_handle *dpb = er_bits_open_read(buf.data(), nbytes);
    for (const plane_event &e : ev) {
        if (e.kind == 0)
            er_ac_dec_symbol(dac, lc3_spectrum_models + e.model);
        else
            er_get_bit(dpb);
    }

    int floor_log2_range = -1;
    for (uint32_t rr = er_ac_dec_range(dac); rr; rr >>= 1)
        floor_log2_range++;
    int spec_left = 8 * nbytes -
        (8 * (er_ac_dec_pos(dac) - 3) + (25 - floor_log2_range)) -
        er_bits_used(dpb);

    int left_ref = 8 * nbytes -
        er_ac_dec_bits_used(dac) - er_bits_used(dpb);
    TASK_CHECK(left_ref >= 0);
    TASK_CHECK(spec_left == left_ref);
    TASK_CHECK(left_prod >= 0);
    TASK_CHECK(left_ref >= left_prod);

    er_ac_dec_close(dac);
    er_bits_close(dpb);
    er_ac_enc_close(ac);
    er_bits_close(pb);
}

static void production_to_standalone(int nbytes)
{
    int plain_bits = 0;
    std::vector<plane_event> ev = build_plane_plan(nbytes, plain_bits);

    std::vector<uint8_t> buf(nbytes, 0);
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_WRITE, buf.data(), nbytes);

    for (const plane_event &e : ev) {
        if (e.kind == 0)
            lc3_put_symbol(&bits, lc3_spectrum_models + e.model, e.value);
        else
            lc3_put_bit(&bits, e.value);
    }
    lc3_flush_bits(&bits);

    er_ac_dec_handle *ac = er_ac_dec_open(buf.data(), nbytes);
    er_bits_handle *pb = er_bits_open_read(buf.data(), nbytes);

    int checked = 0;
    for (const plane_event &e : ev) {
        if (e.kind == 0) {
            int s = er_ac_dec_symbol(ac, lc3_spectrum_models + e.model);
            TASK_CHECK(s == e.value);
        } else {
            TASK_CHECK(er_get_bit(pb) == e.value);
        }
        checked++;
    }
    TASK_CHECK(checked == (int)ev.size());
    TASK_CHECK(er_ac_dec_error(ac) == 0);

    int left_ref = 8 * nbytes -
        er_ac_dec_bits_used(ac) - er_bits_used(pb);
    TASK_CHECK(left_ref >= 0);

    er_ac_dec_close(ac);
    er_bits_close(pb);
}

/* Dedicated shared termination byte case: forward and backward regions
 * meet inside one packet byte.  The sizes are chosen so that exactly one
 * boundary byte is shared and the production round trip still succeeds. */
static void shared_termination_byte(void)
{
    const int nbytes = 24;
    const int nsym = 24;
    const int nplain = 26;

    std::vector<uint8_t> buf(nbytes, 0);

    er_ac_enc_handle *ac = er_ac_enc_open();
    er_bits_handle *pb = er_bits_open_write(buf.data(), nbytes);

    for (int k = 0; k < nsym; k++)
        er_ac_enc_symbol(ac, lc3_spectrum_models + (k % 24), 15);
    for (int k = 0; k < nplain; k++)
        er_put_bit(pb, k & 1);

    int ac_bytes = er_ac_enc_terminate(ac, buf.data(), nbytes, nplain);
    TASK_CHECK(ac_bytes > 0);
    int plain_bytes = (nplain + 7) >> 3;
    TASK_CHECK(ac_bytes + plain_bytes == nbytes + 1); /* one shared byte */

    /* Production serialization of the identical plan must yield the very
     * same bytes, proving the merged boundary byte is interpreted the
     * same way by both writers. */
    std::vector<uint8_t> ref(nbytes, 0);
    lc3_bits_t wbits;
    lc3_setup_bits(&wbits, LC3_BITS_MODE_WRITE, ref.data(), nbytes);
    for (int k = 0; k < nsym; k++)
        lc3_put_symbol(&wbits, lc3_spectrum_models + (k % 24), 15);
    for (int k = 0; k < nplain; k++)
        lc3_put_bit(&wbits, k & 1);
    lc3_flush_bits(&wbits);
    for (int i = 0; i < nbytes; i++)
        TASK_CHECK(buf[i] == ref[i]);

    /* Both readers still recover every event from the merged byte. */
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_READ, buf.data(), nbytes);
    for (int k = 0; k < nsym; k++)
        TASK_CHECK(lc3_get_symbol(&bits,
            lc3_spectrum_models + (k % 24)) == 15);
    for (int k = 0; k < nplain; k++)
        TASK_CHECK(lc3_get_bit(&bits) == (k & 1));
    TASK_CHECK(lc3_check_bits(&bits) == 0);

    er_ac_enc_close(ac);
    er_bits_close(pb);
}

/* Pure plain plane, non byte aligned, crossing the 32 bit accumulator. */
static void plain_accumulator_cross(void)
{
    const int nbytes = 16;
    const int widths[6] = { 7, 13, 17, 11, 9, 13 };   /* sums to 70 */
    std::vector<unsigned> values(6);
    for (unsigned &v : values)
        v = lcg_next();

    /* Standalone writer, production reader. */
    std::vector<uint8_t> buf(nbytes, 0);
    er_bits_handle *pb = er_bits_open_write(buf.data(), nbytes);
    int total = 0;
    for (int k = 0; k < 6; k++) {
        er_put_bits(pb, values[k], widths[k]);
        total += widths[k];
    }
    TASK_CHECK(total > 32);

    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_READ, buf.data(), nbytes);
    for (int k = 0; k < 6; k++) {
        unsigned mask = (1u << widths[k]) - 1;
        TASK_CHECK(lc3_get_bits(&bits, widths[k]) == (values[k] & mask));
    }
    /* The standalone cursor counts exactly `total` plain bits; the
     * production reader additionally holds its 4 byte prefetch, so only
     * the non-negative / no-plane-crossing invariants are asserted. */
    TASK_CHECK(lc3_get_bits_left(&bits) >= 0);
    TASK_CHECK(lc3_check_bits(&bits) == 0);
    er_bits_close(pb);

    /* Production writer, standalone reader. */
    std::vector<uint8_t> buf2(nbytes, 0);
    lc3_bits_t wbits;
    lc3_setup_bits(&wbits, LC3_BITS_MODE_WRITE, buf2.data(), nbytes);
    for (int k = 0; k < 6; k++) {
        unsigned mask = (1u << widths[k]) - 1;
        lc3_put_bits(&wbits, values[k] & mask, widths[k]);
    }
    lc3_flush_bits(&wbits);

    er_bits_handle *rp = er_bits_open_read(buf2.data(), nbytes);
    for (int k = 0; k < 6; k++) {
        unsigned mask = (1u << widths[k]) - 1;
        TASK_CHECK((er_get_bits(rp, widths[k]) & mask) ==
                   (values[k] & mask));
    }
    TASK_CHECK(er_bits_used(rp) == total);
    er_bits_close(rp);
}

static void run_planes(void)
{
    standalone_to_production(64);
    production_to_standalone(64);
    shared_termination_byte();
    plain_accumulator_cross();
    std::printf("planes: carry/pending/shared-byte/unaligned covered\n");
}

/* ------------------------------------------------------------------ */
/* spectrum group                                                      */
/* ------------------------------------------------------------------ */

struct spec_case {
    int dt_us, sr_hz, nbytes;
    int nq, lsb_mode, g_idx;
    std::vector<int> xq;
    const char *tag;
};

static void report_spec_ctx(const spec_case &c)
{
    std::fprintf(stderr,
        "LC3_ENTROPY_MISMATCH duration=%dus rate=%dHz nbytes=%d tag=%s\n",
        c.dt_us, c.sr_hz, c.nbytes, c.tag);
}

/* Build one signed coefficient plan deterministically.  The magnitude
 * ladder guarantees at least two escape levels, unit magnitudes exercise
 * LSB sign recovery, and zero members allow LSB unit injection. */
static std::vector<int> build_coeff_plan(int ne, int nq, int variant)
{
    std::vector<int> x(ne, 0);
    static const int mag[8] = { 1, 3, 5, 8, 17, 20, 33, 64 };
    for (int i = 0; i < nq; i++) {
        int m = mag[(i + variant) % 8];
        int sign = ((i * 7 + variant * 3) & 1) ? -1 : 1;
        if ((i + variant) % 11 == 4)
            m = 0;                       /* explicit zero member */
        if ((i + variant) % 13 == 7)
            m = 1;                       /* LSB unit candidate */
        x[i] = sign * m;
    }
    /* Guarantee a large/zero pair and a large/unit pair for LSB tests. */
    if (nq >= 8) {
        x[4] = 20;
        x[5] = 0;
        x[6] = -1;
        x[7] = 24;
    }
    return x;
}

static float tol_bound(float ref)
{
    return 2e-5f * (1.f + std::fabs(ref));
}

/* Independent feasibility probe: does the forward quantized region for
 * the first nq coefficients of x fit in the packet, leaving at least
 * `reserve` bits for the deferred plane?  Only the standalone coder is
 * used, so the verdict never comes from the function under test. */
static bool forward_fits(int dt, int sr, int nbytes, int high_rate,
    const std::vector<int> &x, int nq, int lsb_mode, int reserve)
{
    int ne = (int)x.size();
    er_write_handle *wh = er_write_open();
    er_write_plan *wp = er_write_get_plan(wh);
    wp->ne = ne;
    wp->nq = nq;
    wp->lsb_mode = lsb_mode;
    wp->high_rate = high_rate;
    for (int i = 0; i < ne; i++) {
        wp->xq[i] = i < nq ? x[i] : 0;
        wp->xg[i] = (float)wp->xq[i];
    }

    std::vector<uint8_t> scratch(nbytes, 0);
    er_ac_enc_handle *ac = er_ac_enc_open();
    er_bits_handle *pb = er_bits_open_write(scratch.data(), nbytes);
    er_spec_write(wh, ac, pb, dt, sr);

    int forward = er_ac_enc_bits_used(ac);
    int backward = er_bits_used(pb);
    bool fits = (forward + backward + reserve) <= 8 * nbytes;

    er_ac_enc_close(ac);
    er_bits_close(pb);
    er_write_close(wh);
    return fits;
}

/* Largest even nq whose forward region fits with the requested reserve. */
static int choose_fitting_nq(int dt, int sr, int nbytes, int high_rate,
    const std::vector<int> &x, int lsb_mode, int reserve)
{
    int ne = (int)x.size();
    int best = 2;
    for (int nq = 2; nq <= ne; nq += 2) {
        if (forward_fits(dt, sr, nbytes, high_rate, x, nq,
                lsb_mode, reserve))
            best = nq;
        else if (nq > 8)
            break;
    }
    return best;
}

/* Tight nq for the deferred-budget exhaustion cases.  The forward region
 * must fit cleanly, while the uncapped deferred demand exceeds the room
 * left after it, so the residual/LSB plane provably stops mid stream. */
static int choose_tight_nq(int dt, int sr, int nbytes, int high_rate,
    const std::vector<int> &x, int lsb_mode)
{
    int ne = (int)x.size();
    int fallback = 2;

    for (int nq = 8; nq <= ne; nq += 2) {
        er_write_handle *wh = er_write_open();
        er_write_plan *wp = er_write_get_plan(wh);
        wp->ne = ne;
        wp->nq = nq;
        wp->lsb_mode = lsb_mode;
        wp->high_rate = high_rate;
        for (int i = 0; i < ne; i++) {
            wp->xq[i] = i < nq ? x[i] : 0;
            wp->xg[i] = (float)wp->xq[i];
        }

        std::vector<uint8_t> scratch(nbytes, 0);
        er_ac_enc_handle *ac = er_ac_enc_open();
        er_bits_handle *pb = er_bits_open_write(scratch.data(), nbytes);
        er_spec_write(wh, ac, pb, dt, sr);

        int forward = er_ac_enc_bits_used(ac);
        int backward = er_bits_used(pb);
        int room = 8 * nbytes - forward - backward;
        int demand = lsb_mode ? er_lsb_demand(wh)
                             : er_residual_demand(wh);

        er_ac_enc_close(ac);
        er_bits_close(pb);
        er_write_close(wh);

        if (room < 0)
            break;                       /* forward itself no longer fits */
        fallback = nq;
        if (demand > room && demand > 0)
            return nq;
    }
    return fallback;
}

/* Decode one production-built spectrum section with both readers and
 * compare every observable quantity. */
static void cross_read_spectrum(const spec_case &c, int expect_trunc)
{
    int dt = dt_of_us(c.dt_us);
    int sr = sr_of_hz(c.sr_hz);
    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);
    int bw = sr;

    int lsb_allow = 0, high_rate = 0;
    bridge_resolve_modes(sr, c.nbytes, &lsb_allow, &high_rate);
    if (c.lsb_mode)
        TASK_CHECK(lsb_allow == 1);

    std::vector<float> xf(ne, 0.f);
    for (int i = 0; i < c.nq; i++)
        xf[i] = (float)c.xq[i];

    /* Noise factor comes from the plan itself, not the decoder. */
    int nf = bridge_estimate_noise(dt, bw, xf.data(), c.nq);

    /* Production serialization of the explicit plan. */
    std::vector<uint8_t> buf(c.nbytes, 0);
    lc3_bits_t wbits;
    lc3_setup_bits(&wbits, LC3_BITS_MODE_WRITE, buf.data(), c.nbytes);
    bridge_put_quantized(&wbits, dt, sr, c.nbytes,
        xf.data(), c.nq, c.lsb_mode);
    int left_write = bridge_get_bits_left(&wbits);

    std::vector<float> xwork = xf;
    if (c.lsb_mode)
        bridge_put_lsb(&wbits, left_write, xwork.data(), c.nq);
    else
        bridge_put_residual(&wbits, left_write, xwork.data(), c.nq);
    lc3_flush_bits(&wbits);

    /* --- Independent decode of the production bytes. --- */
    er_ac_dec_handle *ac = er_ac_dec_open(buf.data(), c.nbytes);
    er_bits_handle *pb = er_bits_open_read(buf.data(), c.nbytes);
    er_spec_handle *sh = er_spec_open();
    char err[160] = {0};
    int rc = er_spec_decode(ac, pb, dt, sr, c.nbytes,
        c.nq, c.lsb_mode, nf, c.g_idx, sh, err, sizeof(err));
    if (rc != 0) {
        report_spec_ctx(c);
        mismatch("reference decode failed: %s", err);
    }
    const er_spec_result *er = er_spec_get(sh);

    /* --- Production static decode stages on a fresh context. --- */
    lc3_bits_t rbits;
    lc3_setup_bits(&rbits, LC3_BITS_MODE_READ,
        const_cast<uint8_t *>(buf.data()), c.nbytes);
    std::vector<float> yp(ne, 0.f);
    uint16_t seed_p = 0;
    if (bridge_get_quantized(&rbits, dt, sr, c.nbytes,
            c.nq, c.lsb_mode, yp.data(), &seed_p) != 0)
        mismatch("production get_quantized rejected planned stream");

    /* Integer snapshot taken before any residual/LSB or gain stage. */
    std::vector<int> yq_int(c.nq);
    for (int i = 0; i < c.nq; i++)
        yq_int[i] = (int)yp[i];

    int left_read = bridge_get_bits_left(&rbits);

    if (c.lsb_mode)
        bridge_get_lsb(&rbits, left_read, yp.data(), c.nq, &seed_p);
    else
        bridge_get_residual(&rbits, left_read, yp.data(), c.nq);

    /* Residual/LSB corrected snapshot, still before the gain scaling. */
    std::vector<float> yr_p(ne, 0.f);
    for (int i = 0; i < c.nq; i++)
        yr_p[i] = yp[i];

    int g_int = c.g_idx - bridge_gain_offset(sr, c.nbytes);
    float g_p = bridge_unquantize_gain(g_int);
    bridge_unquantize(dt, sr, g_int, yp.data(), c.nq);
    bridge_fill_noise(dt, bw, nf, seed_p, g_p, yp.data(), c.nq);

    /* --- Stage by stage exact comparison. --- */
    for (int i = 0; i < c.nq; i++) {
        /* Pre-deferred integer stage: production and reference agree. */
        if (yq_int[i] != er->xq[i]) {
            report_spec_ctx(c);
            mismatch("production quantized at coeff=%d prod=%d ref=%d",
                i, yq_int[i], er->xq[i]);
        }
        /* Post deferred residual/LSB stage: production and reference. */
        if (yr_p[i] != er->xr[i]) {
            report_spec_ctx(c);
            mismatch("residual value at coeff=%d prod=%.4f ref=%.4f",
                i, (double)yr_p[i], (double)er->xr[i]);
        }

        /* Equality with the explicit plan on clean, non-truncated streams.
         *
         * Non-LSB mode carries every integer bit in the quantized stage.
         * LSB mode is allowed to drop trailing low bits when the deferred
         * plane runs out, so full plan equality is only required on the
         * early pairs that are guaranteed to receive LSB bits; the
         * production/reference exact agreement above covers every coeff. */
        if (!expect_trunc && !c.lsb_mode && er->xq[i] != c.xq[i]) {
            report_spec_ctx(c);
            mismatch("quantized value at coeff=%d ref=%d plan=%d",
                i, er->xq[i], c.xq[i]);
        }
    }

    if (!expect_trunc && c.lsb_mode && c.nq >= 8) {
        /* The deliberately placed unit/large pairs sit at coefficients
         * 4..7, early enough to be fully covered by the LSB plane. */
        for (int i = 4; i < 8 && i < c.nq; i++) {
            if ((int)er->xr[i] != c.xq[i]) {
                report_spec_ctx(c);
                mismatch("lsb unit pair at coeff=%d ref=%.1f plan=%d",
                    i, (double)er->xr[i], c.xq[i]);
            }
        }
    }

    /* --- Noise seed, then residual/LSB values and final spectrum. --- */
    if (er->nf_seed != seed_p) {
        report_spec_ctx(c);
        mismatch("noise seed ref=%u prod=%u", er->nf_seed, seed_p);
    }

    for (int i = 0; i < ne; i++) {
        /* Recompute the independent gain-scaled value is already in xg. */
        if (std::fabs(yp[i] - er->xg[i]) > tol_bound(er->xg[i])) {
            report_spec_ctx(c);
            mismatch("first spectral deviation at coeff=%d prod=%.8f "
                "ref=%.8f tol=%.2e", i, (double)yp[i], (double)er->xg[i],
                (double)tol_bound(er->xg[i]));
        }
    }

    /* Branch coverage evidence. */
    TASK_CHECK(er->npairs == c.nq / 2);
    if (c.nq > (ne + 2) / 2)
        TASK_CHECK(er->second_half_pairs > 0);
    TASK_CHECK(er->high_rate == high_rate);

    if (expect_trunc) {
        TASK_CHECK(er->budget_stop == 1);
        TASK_CHECK(er->stop_pair >= 0);
        /* The reference never reads plain bits past the physical plane. */
        TASK_CHECK(er_bits_used(pb) <=
            8 * c.nbytes - er_ac_dec_bits_used(ac));
    }

    er_spec_close(sh);
    er_ac_dec_close(ac);
    er_bits_close(pb);
}

/* Independent writer feeding the production reader: symbols and plain
 * bits generated without any production write routine. */
static void cross_write_spectrum(const spec_case &c)
{
    int dt = dt_of_us(c.dt_us);
    int sr = sr_of_hz(c.sr_hz);
    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    int lsb_allow = 0, high_rate = 0;
    bridge_resolve_modes(sr, c.nbytes, &lsb_allow, &high_rate);

    er_write_handle *wh = er_write_open();
    er_write_plan *wp = er_write_get_plan(wh);
    wp->ne = ne;
    wp->nq = c.nq;
    wp->lsb_mode = c.lsb_mode;
    wp->high_rate = high_rate;
    for (int i = 0; i < ne; i++) {
        wp->xq[i] = i < c.nq ? c.xq[i] : 0;
        wp->xg[i] = (float)wp->xq[i];
    }

    std::vector<uint8_t> buf(c.nbytes, 0);
    er_ac_enc_handle *ac = er_ac_enc_open();
    er_bits_handle *pb = er_bits_open_write(buf.data(), c.nbytes);

    er_spec_write(wh, ac, pb, dt, sr);

    /* Remaining room derived solely from the standalone cursors: total
     * plane minus forward committed bits minus backward consumed bits. */
    int left = 8 * c.nbytes -
        er_ac_enc_bits_used(ac) - er_bits_used(pb);
    if (c.lsb_mode)
        er_lsb_write(wh, pb, left > 0 ? left : 0);
    else
        er_residual_write(wh, pb, left > 0 ? left : 0);

    int ac_bytes = er_ac_enc_terminate(ac, buf.data(), c.nbytes,
        er_bits_used(pb));
    TASK_CHECK(ac_bytes > 0);

    lc3_bits_t rbits;
    lc3_setup_bits(&rbits, LC3_BITS_MODE_READ, buf.data(), c.nbytes);
    std::vector<float> yp(ne, 0.f);
    uint16_t seed_p = 0;
    int rc = bridge_get_quantized(&rbits, dt, sr, c.nbytes,
        c.nq, c.lsb_mode, yp.data(), &seed_p);
    if (rc != 0)
        mismatch("independent-written stream rejected at production read");

    std::vector<int> yq_int(c.nq);
    for (int i = 0; i < c.nq; i++)
        yq_int[i] = (int)yp[i];

    int left_r = bridge_get_bits_left(&rbits);
    if (c.lsb_mode)
        bridge_get_lsb(&rbits, left_r, yp.data(), c.nq, &seed_p);
    else
        bridge_get_residual(&rbits, left_r, yp.data(), c.nq);

    if (c.lsb_mode) {
        /* Low bits are deferred and the trailing ones may be dropped; the
         * early deliberately placed pairs are guaranteed to be covered. */
        for (int i = 4; i < 8 && i < c.nq; i++) {
            if ((int)yp[i] != c.xq[i]) {
                report_spec_ctx(c);
                mismatch("cross-write lsb pair at coeff=%d prod=%d plan=%d",
                    i, (int)yp[i], c.xq[i]);
            }
        }
    } else {
        for (int i = 0; i < c.nq; i++) {
            if (yq_int[i] != c.xq[i]) {
                report_spec_ctx(c);
                mismatch("cross-write quantized at coeff=%d prod=%d plan=%d",
                    i, yq_int[i], c.xq[i]);
            }
        }
    }
    TASK_CHECK(bridge_check_bits(&rbits) == 0);

    er_write_close(wh);
    er_ac_enc_close(ac);
    er_bits_close(pb);
}

static void run_spectrum(void)
{
    struct pick { int dt_us, sr_hz, nbytes, lsb, trunc; const char *tag; };
    const pick picks[] = {
        /* low rate, LSB off, residual correction, two escape levels */
        { 10000, 16000, 26, 0, 0, "lowrate-residual" },
        /* high-rate threshold exceeded (16k limit nbytes > 40) */
        { 10000, 16000, 60, 0, 0, "highrate-on" },
        /* high-rate threshold not exceeded, boundary value nbytes == 40 */
        { 10000, 16000, 40, 0, 0, "highrate-off-boundary" },
        /* LSB enabled (16k limit nbytes >= 80): deferred unit +/-1 */
        { 10000, 16000, 85, 1, 0, "lsb-units" },
        /* 7.5 ms across rates, second half model switch; LSB limits 100.. */
        {  7500, 24000, 110, 1, 0, "7m5-24k-lsb" },
        {  7500, 32000, 130, 1, 0, "7m5-32k-lsb" },
        { 10000, 48000, 150, 1, 0, "10m-48k-lsb" },
        {  7500,  8000, 30, 0, 0, "7m5-8k-residual" },
        /* deliberately tight budget, stop inside the deferred region */
        { 10000, 16000, 24, 0, 1, "truncated-residual" },
        /* LSB legal from 100 bytes at 24k; tight nq exhausts LSB plane */
        { 10000, 24000, 105, 1, 1, "truncated-lsb" },
    };

    int saw_escape2 = 0, saw_residual = 0, saw_unit_pos = 0;
    int saw_unit_neg = 0, saw_second_half = 0, saw_trunc = 0;

    for (int variant = 0; variant < (int)(sizeof(picks)/sizeof(picks[0]));
         variant++) {
        const pick &pk = picks[variant];
        int dt = dt_of_us(pk.dt_us);
        int sr = sr_of_hz(pk.sr_hz);
        int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

        int lsb_allow = 0, high_rate = 0;
        bridge_resolve_modes(sr, pk.nbytes, &lsb_allow, &high_rate);

        /* Plan over the full coefficient span first, then pick a legal
         * coded length from the standalone feasibility probes. */
        std::vector<int> full = build_coeff_plan(ne, ne, variant);
        int nq;
        if (pk.trunc)
            nq = choose_tight_nq(dt, sr, pk.nbytes, high_rate,
                full, pk.lsb);
        else
            nq = choose_fitting_nq(dt, sr, pk.nbytes, high_rate,
                full, pk.lsb, 24);
        std::vector<int> x = full;
        TASK_CHECK(nq >= 8);

        spec_case c{pk.dt_us, pk.sr_hz, pk.nbytes, nq, pk.lsb,
            100 + variant, x, pk.tag};

        cross_read_spectrum(c, pk.trunc);
        if (!pk.trunc)
            cross_write_spectrum(c);

        /* Re-derive coverage counters from a fresh independent decode. */
        std::vector<float> xf(ne, 0.f);
        for (int i = 0; i < nq; i++)
            xf[i] = (float)x[i];
        int nf = bridge_estimate_noise(dt, sr, xf.data(), nq);
        std::vector<uint8_t> buf(pk.nbytes, 0);
        lc3_bits_t wb;
        lc3_setup_bits(&wb, LC3_BITS_MODE_WRITE, buf.data(), pk.nbytes);
        bridge_put_quantized(&wb, dt, sr, pk.nbytes,
            xf.data(), nq, pk.lsb);
        int lw = bridge_get_bits_left(&wb);
        std::vector<float> xw = xf;
        if (pk.lsb) bridge_put_lsb(&wb, lw, xw.data(), nq);
        else bridge_put_residual(&wb, lw, xw.data(), nq);
        lc3_flush_bits(&wb);

        er_ac_dec_handle *ac = er_ac_dec_open(buf.data(), pk.nbytes);
        er_bits_handle *pb = er_bits_open_read(buf.data(), pk.nbytes);
        er_spec_handle *sh = er_spec_open();
        char err[160] = {0};
        TASK_CHECK(er_spec_decode(ac, pb, dt, sr, pk.nbytes,
            nq, pk.lsb, nf, c.g_idx, sh, err, sizeof(err)) == 0);
        const er_spec_result *er = er_spec_get(sh);

        if (er->max_escape >= 2) saw_escape2 = 1;
        if (er->residual_applied > 0) saw_residual = 1;
        if (er->lsb_unit_pos > 0) saw_unit_pos = 1;
        if (er->lsb_unit_neg > 0) saw_unit_neg = 1;
        if (er->second_half_pairs > 0) saw_second_half = 1;
        if (er->budget_stop) saw_trunc = 1;

        er_spec_close(sh);
        er_ac_dec_close(ac);
        er_bits_close(pb);
    }

    TASK_CHECK(saw_escape2);
    TASK_CHECK(saw_residual);
    TASK_CHECK(saw_unit_pos);
    TASK_CHECK(saw_unit_neg);
    TASK_CHECK(saw_second_half);
    TASK_CHECK(saw_trunc);
    std::printf("spectrum: escapes/residual/lsb-units/half/trunc covered\n");
}

/* ------------------------------------------------------------------ */
/* frames group                                                        */
/* ------------------------------------------------------------------ */

struct frame_plan {
    int dt_us, sr_hz, nbytes;
    int nq, lsb_mode, g_idx, nf, bw;
    int tns_order[2];
    int tns_rc[2][8];
    std::vector<int> xq;
};

static float frame_tol(float ref) { return 2e-5f * (1.f + std::fabs(ref)); }

/* Construct a complete legal packet.  Non-target fields use the
 * production SNS/TNS writers; the spectrum payload is serialized from
 * the explicit plan, while every expected value comes from the
 * independent reference parse. */
static std::vector<uint8_t> build_frame_packet(const frame_plan &fp)
{
    int dt = dt_of_us(fp.dt_us);
    int sr = sr_of_hz(fp.sr_hz);
    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    std::vector<uint8_t> buf(fp.nbytes, 0);
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_WRITE, buf.data(), fp.nbytes);

    lc3_bwdet_put_bw(&bits, (enum lc3_srate)sr,
        (enum lc3_bandwidth)fp.bw);

    lc3_spec_side_t side;
    side.nq = fp.nq;
    side.lsb_mode = fp.lsb_mode;
    side.g_idx = fp.g_idx;
    bridge_spec_put_side(&bits, dt, sr, &side);

    lc3_tns_data_t tns;
    std::memset(&tns, 0, sizeof(tns));
    tns.nfilters = 1 + (dt >= LC3_DT_5M && fp.bw >= LC3_BANDWIDTH_SWB);
    tns.lpc_weighting = fp.nbytes * 8 < 120 * (1 + dt);
    for (int f = 0; f < 2; f++) {
        tns.rc_order[f] = fp.tns_order[f];
        for (int i = 0; i < 8; i++)
            tns.rc[f][i] = fp.tns_rc[f][i];
    }
    lc3_tns_put_data(&bits, &tns);

    lc3_put_bit(&bits, 0);              /* pitch present = false */

    lc3_sns_data_t sns;
    std::memset(&sns, 0, sizeof(sns)); /* legal all-zero SNS plan */
    lc3_sns_put_data(&bits, &sns);

    lc3_put_bits(&bits, fp.nf, 3);

    std::vector<float> xf(ne, 0.f);
    for (int i = 0; i < fp.nq; i++)
        xf[i] = (float)fp.xq[i];
    bridge_put_quantized(&bits, dt, sr, fp.nbytes,
        xf.data(), fp.nq, fp.lsb_mode);
    int left = bridge_get_bits_left(&bits);
    std::vector<float> xw = xf;
    if (fp.lsb_mode)
        bridge_put_lsb(&bits, left, xw.data(), fp.nq);
    else
        bridge_put_residual(&bits, left, xw.data(), fp.nq);

    lc3_flush_bits(&bits);
    return buf;
}

static er_frame_side plan_to_side(const frame_plan &fp)
{
    int dt = dt_of_us(fp.dt_us);
    er_frame_side s;
    std::memset(&s, 0, sizeof(s));
    s.dt = dt;
    s.sr = sr_of_hz(fp.sr_hz);
    s.nbytes = fp.nbytes;
    s.bw = fp.bw;
    s.nq = fp.nq;
    s.lsb_mode = fp.lsb_mode;
    s.g_idx = fp.g_idx;
    s.nf = fp.nf;
    s.pitch_present = 0;
    s.tns.nfilters = 1 + (dt >= LC3_DT_5M && fp.bw >= LC3_BANDWIDTH_SWB);
    s.tns.lpc_weighting = fp.nbytes * 8 < 120 * (1 + dt);
    for (int f = 0; f < 2; f++) {
        s.tns.order[f] = fp.tns_order[f];
        for (int i = 0; i < 8; i++)
            s.tns.rc[f][i] = fp.tns_rc[f][i];
    }
    return s;
}

static void check_trace_vs_reference(const frame_plan &fp,
    const std::vector<uint8_t> &buf)
{
    int dt = dt_of_us(fp.dt_us);
    int sr = sr_of_hz(fp.sr_hz);
    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    /* Independent parse from byte 0, own cursors only. */
    er_ac_dec_handle *ac = er_ac_dec_open(buf.data(), fp.nbytes);
    er_bits_handle *pb = er_bits_open_read(buf.data(), fp.nbytes);
    er_frame_handle *fh = er_frame_open();
    er_frame_side side = plan_to_side(fp);
    char err[160] = {0};
    if (er_frame_parse(ac, pb, &side, fh, err, sizeof(err)) != 0)
        mismatch("frame reference parse failed: %s", err);
    const er_frame_result *fr = er_frame_get(fh);
    TASK_CHECK(fr->ac_after_tns_pos > 3);     /* TNS prefix advanced AC */

    /* Public decode path, trace captured inside lc3_spec_decode. */
    bridge_trace_reset();
    void *mem = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    lc3_decoder_t dec = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, mem);
    TASK_CHECK(dec != nullptr);
    std::vector<float> pcm(2 * ne, 123.f);
    int ret = lc3_decode(dec, buf.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, pcm.data(), 2);
    TASK_CHECK(ret == 0);
    TASK_CHECK(bridge_trace_count() == 1);

    float snap[ER_MAX_NE];
    int sdt = -1, ssr = -1, snq = -1;
    TASK_CHECK(bridge_trace_copy(0, snap, ne, &sdt, &ssr, &snq) == 0);
    TASK_CHECK(sdt == dt && ssr == sr && snq == fp.nq);

    int first_bad = -1;
    for (int i = 0; i < ne; i++) {
        if (std::fabs(snap[i] - fr->spec.xg[i]) >
            frame_tol(fr->spec.xg[i])) {
            first_bad = i;
            break;
        }
    }
    if (first_bad >= 0) {
        mismatch("trace deviation duration=%dus rate=%d nbytes=%d coeff=%d "
            "prod=%.8f ref=%.8f", fp.dt_us, fp.sr_hz, fp.nbytes, first_bad,
            (double)snap[first_bad], (double)fr->spec.xg[first_bad]);
    }

    /* Output count and stride-2 guard region. */
    int ns = lc3_frame_samples(fp.dt_us, fp.sr_hz);
    for (int i = 0; i < ns; i++)
        TASK_CHECK(pcm[2 * i + 1] == 123.f);

    int longest = 0, run = 0;
    for (int i = 0; i < ns; i++) {
        if (pcm[2 * i] != 0.f && std::isfinite(pcm[2 * i]))
            run++;
        else
            run = 0;
        if (run > longest)
            longest = run;
    }
    TASK_CHECK(longest > ns / 4);

    std::free(mem);
    er_frame_close(fh);
    er_ac_dec_close(ac);
    er_bits_close(pb);
}

/* Two decoders interleaved with stride 2 must match independent
 * single-instance decodes. */
static void check_interleaved_instances(const frame_plan &fp,
    const std::vector<uint8_t> &buf)
{
    int ns = lc3_frame_samples(fp.dt_us, fp.sr_hz);

    void *m0 = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    void *m1 = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    lc3_decoder_t d0 = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, m0);
    lc3_decoder_t d1 = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, m1);

    std::vector<float> inter(2 * ns, 0.f);
    TASK_CHECK(lc3_decode(d0, buf.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, inter.data(), 2) == 0);
    TASK_CHECK(lc3_decode(d1, buf.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, inter.data() + 1, 2) == 0);

    void *mr = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    lc3_decoder_t dr = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, mr);
    std::vector<float> ref(ns, 0.f);
    TASK_CHECK(lc3_decode(dr, buf.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, ref.data(), 1) == 0);

    for (int i = 0; i < ns; i++) {
        TASK_CHECK(inter[2 * i] == ref[i]);
        TASK_CHECK(inter[2 * i + 1] == ref[i]);
    }

    std::free(m0);
    std::free(m1);
    std::free(mr);
}

/* Explicitly runaway escape stream: the decoder walks escape symbols
 * until the depth cap and must report failure, driving the public entry
 * into PLC.  This is a constructed exhaustion, not an assumed bit flip. */
static std::vector<uint8_t> build_exhausted_packet(const frame_plan &fp)
{
    int dt = dt_of_us(fp.dt_us);
    int sr = sr_of_hz(fp.sr_hz);
    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    std::vector<uint8_t> buf(fp.nbytes, 0);
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_WRITE, buf.data(), fp.nbytes);

    lc3_bwdet_put_bw(&bits, (enum lc3_srate)sr,
        (enum lc3_bandwidth)fp.bw);
    lc3_spec_side_t side;
    side.nq = ne;
    side.lsb_mode = false;
    side.g_idx = fp.g_idx;
    bridge_spec_put_side(&bits, dt, sr, &side);

    lc3_tns_data_t tns;
    std::memset(&tns, 0, sizeof(tns));
    tns.nfilters = 1 + (dt >= LC3_DT_5M && fp.bw >= LC3_BANDWIDTH_SWB);
    lc3_tns_put_data(&bits, &tns);
    lc3_put_bit(&bits, 0);
    lc3_sns_data_t sns;
    std::memset(&sns, 0, sizeof(sns));
    lc3_sns_put_data(&bits, &sns);
    lc3_put_bits(&bits, fp.nf, 3);

    /* Emit escape symbol 16 far beyond the 14 level depth cap. */
    for (int k = 0; k < ne; k++)
        lc3_put_symbol(&bits, lc3_spectrum_models, 16);

    lc3_flush_bits(&bits);
    return buf;
}

static void check_plc_transfer(const frame_plan &fp,
    const std::vector<uint8_t> &good)
{
    int ns = lc3_frame_samples(fp.dt_us, fp.sr_hz);

    void *ma = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    void *mb = std::malloc(lc3_decoder_size(fp.dt_us, fp.sr_hz));
    lc3_decoder_t da = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, ma);
    lc3_decoder_t db = lc3_setup_decoder(fp.dt_us, fp.sr_hz, 0, mb);

    /* Identical good history on both instances. */
    for (int f = 0; f < 3; f++) {
        std::vector<float> pa(ns), pb(ns);
        TASK_CHECK(lc3_decode(da, good.data(), fp.nbytes,
            LC3_PCM_FORMAT_FLOAT, pa.data(), 1) == 0);
        TASK_CHECK(lc3_decode(db, good.data(), fp.nbytes,
            LC3_PCM_FORMAT_FLOAT, pb.data(), 1) == 0);
    }

    std::vector<uint8_t> bad = build_exhausted_packet(fp);
    std::vector<float> lost_a(ns), lost_b(ns);
    int ra = lc3_decode(da, bad.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, lost_a.data(), 1);
    int rb = lc3_decode(db, nullptr, fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, lost_b.data(), 1);
    TASK_CHECK(ra == 1);
    TASK_CHECK(rb == 1);
    for (int i = 0; i < ns; i++)
        TASK_CHECK(lost_a[i] == lost_b[i]);

    /* The following good frame must converge on both histories too. */
    std::vector<float> na(ns), nb(ns);
    TASK_CHECK(lc3_decode(da, good.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, na.data(), 1) == 0);
    TASK_CHECK(lc3_decode(db, good.data(), fp.nbytes,
        LC3_PCM_FORMAT_FLOAT, nb.data(), 1) == 0);
    for (int i = 0; i < ns; i++)
        TASK_CHECK(na[i] == nb[i]);

    std::free(ma);
    std::free(mb);
}

static void run_frames(void)
{
    frame_plan fp;
    fp.dt_us = 10000;
    fp.sr_hz = 32000;
    fp.nbytes = 80;
    fp.lsb_mode = 1;
    fp.g_idx = 140;
    fp.nf = 3;
    fp.bw = LC3_BANDWIDTH_SWB;
    fp.tns_order[0] = 2;
    fp.tns_order[1] = 0;
    fp.tns_rc[0][0] = 1;
    fp.tns_rc[0][1] = -2;
    for (int i = 2; i < 8; i++)
        fp.tns_rc[0][i] = 0;
    for (int i = 0; i < 8; i++)
        fp.tns_rc[1][i] = 0;

    int ne = lc3_ne(LC3_DT_10M, LC3_SRATE_32K);
    fp.nq = 64;
    fp.xq.assign(ne, 0);
    static const int mag[8] = { 1, 4, 7, 12, 18, 25, 33, 40 };
    for (int i = 0; i < fp.nq; i++)
        fp.xq[i] = ((i & 1) ? -1 : 1) * mag[i % 8];
    fp.xq[5] = 0;
    fp.xq[6] = 1;

    std::vector<uint8_t> packet = build_frame_packet(fp);
    check_trace_vs_reference(fp, packet);
    check_interleaved_instances(fp, packet);
    check_plc_transfer(fp, packet);
    std::printf("frames: tns-prefix/trace/stride/interleave/plc covered\n");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void task_run(const char *group)
{
    std::string g = group ? group : "";
    if (g == "planes")
        run_planes();
    else if (g == "spectrum")
        run_spectrum();
    else if (g == "frames")
        run_frames();
    else
        mismatch("unknown group '%s'", group);
}
