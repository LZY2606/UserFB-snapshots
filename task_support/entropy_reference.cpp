/*
 * Independent entropy reference model for the LC3 spectral entropy tests.
 *
 * This file is deliberately self contained:
 *  - the packet byte array is owned here,
 *  - plain bits are served by a cursor that walks from the last byte
 *    towards the first one, LSB first,
 *  - the arithmetic coder is a standalone 24-bit interval coder,
 *  - symbols are decided by a linear search across model intervals.
 *
 * Nothing from the production bitstream/spec readers, writers, budget or
 * termination functions is called.  The probability tables and the spectral
 * context lookup in tables.c are format data and are reused read only.
 *
 * The interface exposed here has C linkage on purpose, so that the C++ test
 * driver in target.cpp can redeclare it without any extra header.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

extern "C" {
#include "bits.h"
#include "tables.h"
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int er_floorlog2(uint32_t v)
{
    int n = -1;
    for ( ; v; v >>= 1)
        n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Plain bit cursor, walking from the end of the byte array           */
/* ------------------------------------------------------------------ */

struct er_bits {
    uint8_t *buf;          /* null on a read only view */
    const uint8_t *cbuf;
    int nbytes;
    int used;              /* number of plain bits consumed or written */
};

extern "C" {

struct er_bits_handle { er_bits d; };

er_bits_handle *er_bits_open_read(const uint8_t *data, int nbytes)
{
    auto *h = new er_bits_handle;
    h->d.buf = nullptr;
    h->d.cbuf = data;
    h->d.nbytes = nbytes;
    h->d.used = 0;
    return h;
}

er_bits_handle *er_bits_open_write(uint8_t *data, int nbytes)
{
    auto *h = new er_bits_handle;
    h->d.buf = data;
    h->d.cbuf = data;
    h->d.nbytes = nbytes;
    h->d.used = 0;
    return h;
}

void er_bits_close(er_bits_handle *h) { delete h; }

static int er_plain_get(er_bits *b)
{
    int idx = b->nbytes - 1 - (b->used >> 3);
    int bit = 0;
    if (idx >= 0)
        bit = (b->cbuf[idx] >> (b->used & 7)) & 1;
    b->used++;
    return bit;
}

static void er_plain_put(er_bits *b, int v)
{
    int idx = b->nbytes - 1 - (b->used >> 3);
    if (idx >= 0) {
        uint8_t mask = (uint8_t)(1u << (b->used & 7));
        if (v)
            b->buf[idx] |= mask;
        else
            b->buf[idx] &= (uint8_t)~mask;
    }
    b->used++;
}

int er_get_bit(er_bits_handle *h) { return er_plain_get(&h->d); }

unsigned er_get_bits(er_bits_handle *h, int n)
{
    unsigned v = 0;
    for (int k = 0; k < n; k++)
        v |= (unsigned)er_plain_get(&h->d) << k;
    return v;
}

void er_put_bit(er_bits_handle *h, int v) { er_plain_put(&h->d, v); }

void er_put_bits(er_bits_handle *h, unsigned v, int n)
{
    for (int k = 0; k < n; k++)
        er_plain_put(&h->d, (v >> k) & 1);
}

int er_bits_used(const er_bits_handle *h) { return h->d.used; }

/* Logical number of plain bits still available in the backward region. */
int er_bits_plain_left(const er_bits_handle *h)
{
    return 8 * h->d.nbytes - h->d.used;
}

} /* extern C */

/* ------------------------------------------------------------------ */
/* Standalone 24-bit arithmetic interval decoder                      */
/* ------------------------------------------------------------------ */

struct er_ac_dec {
    const uint8_t *p, *end;
    uint32_t low, range;
    int pos;               /* bytes consumed, including the 3 initial */
    int error;
};

extern "C" {

struct er_ac_dec_handle { er_ac_dec d; };

er_ac_dec_handle *er_ac_dec_open(const uint8_t *data, int nbytes)
{
    auto *h = new er_ac_dec_handle;
    h->d.p = data + (3 <= nbytes ? 3 : nbytes);
    h->d.end = data + nbytes;
    h->d.pos = 3 <= nbytes ? 3 : nbytes;
    uint32_t b0 = nbytes > 0 ? data[0] : 0;
    uint32_t b1 = nbytes > 1 ? data[1] : 0;
    uint32_t b2 = nbytes > 2 ? data[2] : 0;
    h->d.low = (b0 << 16) | (b1 << 8) | b2;
    h->d.range = 0xffffff;
    h->d.error = 0;
    return h;
}

void er_ac_dec_close(er_ac_dec_handle *h) { delete h; }

int er_ac_dec_symbol(er_ac_dec_handle *h, const lc3_ac_model *model)
{
    er_ac_dec *a = &h->d;
    uint32_t r = a->range >> 10;

    if (a->low >= (r << 10))
        a->error = 1;

    /* Linear interval search: largest symbol whose cumulative low bound
     * is not greater than the current offset. */
    int s = 16;
    for ( ; s > 0 && a->low < r * model->s[s].low; s--)
        ;

    a->low -= r * model->s[s].low;
    a->range = r * model->s[s].range;

    while (a->range < 0x10000) {
        a->range <<= 8;
        uint32_t byte = a->p < a->end ? *a->p++ : 0;
        a->pos++;
        a->low = ((a->low << 8) | byte) & 0xffffff;
    }
    return s;
}

int er_ac_dec_pos(const er_ac_dec_handle *h) { return h->d.pos; }
uint32_t er_ac_dec_range(const er_ac_dec_handle *h) { return h->d.range; }
uint32_t er_ac_dec_low(const er_ac_dec_handle *h) { return h->d.low; }
int er_ac_dec_error(const er_ac_dec_handle *h) { return h->d.error; }

/* Number of forward bits currently committed, including the fractional
 * part still held by the interval register. */
int er_ac_dec_bits_used(const er_ac_dec_handle *h)
{
    return 8 * (h->d.pos - 3) + (25 - er_floorlog2(h->d.range));
}

} /* extern C */

/* ------------------------------------------------------------------ */
/* Standalone 24-bit arithmetic interval encoder                      */
/* ------------------------------------------------------------------ */

struct er_ac_enc {
    uint32_t low, range;
    int cache, carry, carry_count;
    std::vector<uint8_t> out;   /* bytes already flushed */
    int symbols;
    int max_carry_count;
    int carry_events;
};

extern "C" {

struct er_ac_enc_handle { er_ac_enc d; };

er_ac_enc_handle *er_ac_enc_open(void)
{
    auto *h = new er_ac_enc_handle;
    h->d.low = 0;
    h->d.range = 0xffffff;
    h->d.cache = -1;
    h->d.carry = 0;
    h->d.carry_count = 0;
    h->d.symbols = 0;
    h->d.max_carry_count = 0;
    h->d.carry_events = 0;
    return h;
}

void er_ac_enc_close(er_ac_enc_handle *h) { delete h; }
int er_ac_enc_symbols(const er_ac_enc_handle *h) { return h->d.symbols; }
int er_ac_enc_max_pending(const er_ac_enc_handle *h) { return h->d.max_carry_count; }
int er_ac_enc_carry_events(const er_ac_enc_handle *h) { return h->d.carry_events; }
int er_ac_enc_pending(const er_ac_enc_handle *h) { return h->d.carry_count; }
int er_ac_enc_low_ff(const er_ac_enc_handle *h)
{
    return h->d.low >= 0xff0000u && !h->d.carry;
}

/* Forward bits already committed: flushed bytes plus the fractional
 * interval register, the held cache byte and the pending 0xff bytes. */
int er_ac_enc_bits_used(const er_ac_enc_handle *h)
{
    const er_ac_enc *a = &h->d;
    int range_bits = er_floorlog2(a->range) + 1;
    int pending = 26 - range_bits +
        (a->cache >= 0) * 8 + a->carry_count * 8;
    return 8 * (int)a->out.size() + pending;
}

} /* extern C */

static void er_enc_shift(er_ac_enc *a)
{
    if (a->low < 0xff0000u || a->carry) {
        if (a->cache >= 0)
            a->out.push_back((uint8_t)(a->cache + a->carry));
        for ( ; a->carry_count > 0; a->carry_count--) {
            a->out.push_back((uint8_t)(a->carry ? 0x00 : 0xff));
            if (!a->carry)
                a->carry_events++;
        }
        a->cache = (int)(a->low >> 16);
        a->carry = 0;
    } else {
        a->carry_count++;
        if (a->carry_count > a->max_carry_count)
            a->max_carry_count = a->carry_count;
    }
    a->low = (a->low << 8) & 0xffffff;
}

extern "C" void er_ac_enc_symbol(
    er_ac_enc_handle *h, const lc3_ac_model *model, int s)
{
    er_ac_enc *a = &h->d;
    uint32_t r = a->range >> 10;

    a->low += r * model->s[s].low;
    a->range = r * model->s[s].range;

    a->carry |= (int)(a->low >> 24);
    a->low &= 0xffffff;

    while (a->range < 0x10000) {
        er_enc_shift(a);
        a->range <<= 8;
    }
    h->d.symbols++;
}

/* Materialize the termination.  The last arithmetic byte shares the
 * packet byte that may already hold backward plain bits; the merge uses
 * the same mask discipline the format defines. */
extern "C" int er_ac_enc_terminate(
    er_ac_enc_handle *h, uint8_t *buf, int nbytes, int plain_nbits)
{
    er_ac_enc *a = &h->d;

    int range_bits = er_floorlog2(a->range) + 1;
    int nbits = 25 - range_bits;
    uint32_t mask = 0xffffffu >> nbits;
    uint32_t val = a->low + mask;
    uint32_t high = a->low + a->range;

    bool over_val = (val >> 24) != 0;
    bool over_high = (high >> 24) != 0;

    val &= 0xffffff;
    high &= 0xffffff;
    val &= ~mask;

    if (over_val == over_high) {
        if (val + mask >= high) {
            nbits++;
            mask >>= 1;
            val = ((a->low + mask) & 0xffffff) & ~mask;
        }
        if (val < a->low)
            a->carry = 1;
    }

    a->low = val;

    for ( ; nbits > 8; nbits -= 8)
        er_enc_shift(a);
    er_enc_shift(a);

    int end_val = a->cache >> (8 - nbits);

    if (a->carry_count) {
        a->out.push_back((uint8_t)a->cache);
        for ( ; a->carry_count > 1; a->carry_count--)
            a->out.push_back(0xff);
        end_val = nbits < 8 ? 0 : 0xff;
    }

    /* Flush committed bytes, then merge the final fractional byte. */
    for (size_t i = 0; i < a->out.size(); i++)
        if (i < (size_t)nbytes)
            buf[i] = a->out[i];

    size_t last = a->out.size();
    if ((int)last < nbytes) {
        buf[last] &= (uint8_t)(0xff >> nbits);
        buf[last] |= (uint8_t)(end_val << (8 - nbits));
    }

    int ac_bytes = (int)last + 1;
    int plain_bytes = (plain_nbits + 7) >> 3;
    /* The forward termination byte and the backward region are allowed to
     * share exactly one boundary byte: the termination keeps the high
     * bits while the backward cursor owns the low ones.  Anything more is
     * a real overflow. */
    if (ac_bytes + plain_bytes > nbytes + 1)
        return -ac_bytes;

    /* Positive return value: number of bytes committed by the forward
     * arithmetic region, including the shared termination byte. */
    return ac_bytes;
}

/* ------------------------------------------------------------------ */
/* Spectral entropy reference                                         */
/* ------------------------------------------------------------------ */

/* Maximum spectral size supported by the non-HR configurations. */
#define ER_MAX_NE 480

struct er_pair_trace {
    int index;             /* pair position */
    int half;              /* 0: first spectral half, 1: second */
    int state_before, state_after;
    int escapes;           /* number of escape symbols read */
    int models[16];        /* model index per symbol */
    int symbols[16];       /* symbol values */
    int nsym;
    int sign_pos[2];       /* plain bit position of each sign, -1 if none */
    int esc_bits;          /* plain LSB bits consumed while escaping */
    int defer_bits;        /* plain residual / deferred LSB bits on pair */
    int u, v;              /* decoded magnitudes */
    int su, sv;            /* decoded signs, 0 positive / 1 negative */
};

struct er_spec_result {
    int ne, nq;
    int lsb_mode, high_rate;
    int nf;
    int g_idx;
    uint16_t nf_seed;

    int xq[ER_MAX_NE];     /* integer quantized values */
    float xr[ER_MAX_NE];   /* values after residual or LSB correction */
    float xg[ER_MAX_NE];   /* final, noise filled and gain scaled */

    int npairs;
    er_pair_trace pairs[ER_MAX_NE / 2];

    int max_escape;
    int second_half_pairs;
    int residual_applied;
    int lsb_unit_pos, lsb_unit_neg;
    int lsb_pairs;
    int budget_stop;       /* 1: stopped with no bit left */
    int stop_pair;         /* pair where exhaustion was observed */
    int stop_phase;        /* 0 symbol, 1 lsb bit, 2 sign bit */
    int bits_left_at_entry;
    int bits_left_at_end;
};

extern "C" {

struct er_spec_handle { er_spec_result r; };

er_spec_handle *er_spec_open(void) { return new er_spec_handle; }
void er_spec_close(er_spec_handle *h) { delete h; }
const er_spec_result *er_spec_get(const er_spec_handle *h) { return &h->r; }

} /* extern C */

static int er_resolve_high_rate(int sr, int nbytes)
{
    int sr_ind = sr;   /* non-HR modes only, sr in 0..4 */
    return (nbytes > 20 * (1 + sr_ind)) && (sr < LC3_SRATE_48K_HR);
}

int er_lsb_allowed(int sr, int nbytes)
{
    int sr_ind = sr;
    return (nbytes >= 20 * (3 + sr_ind)) && (sr < LC3_SRATE_48K_HR);
}

int er_gain_offset(int sr, int nbytes)
{
    int sr_ind = sr;
    int g_off = (nbytes * 8) / (10 * (1 + sr_ind));
    g_off = (g_off < 115 ? g_off : 115);
    int lim = 255;
    return (lim < 105 + 5 * (1 + sr_ind) + g_off ?
            lim : 105 + 5 * (1 + sr_ind) + g_off);
}

int er_nbits_nq(int ne)
{
    return 4 + (ne > 32) + (ne > 64) + (ne > 128) +
             (ne > 256) + (ne > 512);
}

/* Full reference decode of one spectral section. */
extern "C" int er_spec_decode(
    er_ac_dec_handle *ac_h, er_bits_handle *pb_h,
    int dt, int sr, int nbytes, int nq, int lsb_mode, int nf, int g_idx,
    er_spec_handle *out_h, char *err, int errlen)
{
    er_spec_result *r = &out_h->r;

    const int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);
    const int high_rate = er_resolve_high_rate(sr, nbytes);
    const int max_shl = 14;

    std::memset(r, 0, sizeof(*r));
    r->ne = ne;
    r->nq = nq;
    r->lsb_mode = lsb_mode;
    r->high_rate = high_rate;
    r->nf = nf;
    r->g_idx = g_idx;
    r->stop_pair = -1;
    r->stop_phase = -1;

    uint16_t seed = 0;
    uint8_t state = 0;
    int pair_count = 0;

    auto log_fail = [&](const char *m) {
        if (err && errlen)
            std::snprintf(err, errlen, "%s", m);
    };

    for (int h = 0, i = 0; h < 2; h++) {
        const uint8_t (*lut_coeff)[4] = lc3_spectrum_lookup[high_rate][h];
        int limit = (nq < (ne + 2) >> (1 - h)) ? nq : (ne + 2) >> (1 - h);

        for ( ; i < limit; i += 2) {
            er_pair_trace *t = &r->pairs[pair_count];
            t->index = i;
            t->half = h;
            t->state_before = state;
            t->sign_pos[0] = t->sign_pos[1] = -1;

            const uint8_t *lut = lut_coeff[state];
            int u = 0, v = 0, k = 0, shl = 0;

            int s = er_ac_dec_symbol(ac_h, lc3_spectrum_models + lut[k]);
            t->models[t->nsym] = lut[k];
            t->symbols[t->nsym] = s;
            t->nsym++;

            if (lsb_mode && s >= 16) {
                s = er_ac_dec_symbol(ac_h,
                    lc3_spectrum_models + lut[++k]);
                t->models[t->nsym] = lut[k];
                t->symbols[t->nsym] = s;
                t->nsym++;
                shl++;
            }

            for ( ; s >= 16 && shl < max_shl; shl++) {
                int bu = er_get_bit(pb_h);
                int bv = er_get_bit(pb_h);
                t->esc_bits += 2;
                u |= bu << shl;
                v |= bv << shl;

                k += (k < 3);
                s = er_ac_dec_symbol(ac_h,
                    lc3_spectrum_models + lut[k]);
                t->models[t->nsym] = lut[k];
                t->symbols[t->nsym] = s;
                t->nsym++;
            }

            if (s >= 16) {
                log_fail("escape depth exceeded 14 levels");
                return -1;
            }
            if (t->nsym > (int)(sizeof(t->symbols)/sizeof(t->symbols[0]))) {
                log_fail("symbol trace overflow");
                return -1;
            }

            int a = s % 4;
            int b = s / 4;
            u |= a << shl;
            v |= b << shl;

            if (u) {
                t->sign_pos[0] = er_bits_used(pb_h);
                if (er_get_bit(pb_h))
                    t->su = 1;
            }
            if (v) {
                t->sign_pos[1] = er_bits_used(pb_h);
                if (er_get_bit(pb_h))
                    t->sv = 1;
            }

            t->u = u;
            t->v = v;
            t->escapes = shl;
            if (shl > r->max_escape)
                r->max_escape = shl;
            if (h == 1)
                r->second_half_pairs++;

            r->xq[i + 0] = t->su ? -u : u;
            r->xq[i + 1] = t->sv ? -v : v;

            seed = (uint16_t)((seed + (u & 0x7fff) * i +
                               (v & 0x7fff) * (i + 1)) & 0xffff);

            state = (uint8_t)((state << 4) +
                (k > 1 ? 12 + k : 1 + (a + b) * (k + 1)));
            t->state_after = state;

            pair_count++;
        }
    }
    r->npairs = pair_count;
    r->nf_seed = seed;

    /* Logical budget shared by the residual / LSB plain region.  The
     * accounting matches lc3_get_bits_left(): total bits minus the ones
     * already committed by the interval coder and the backward cursor. */
    int ac_used = er_ac_dec_bits_used(ac_h);
    int plain_used = er_bits_used(pb_h);
    int nbits_left = 8 * nbytes - ac_used - plain_used;
    r->bits_left_at_entry = nbits_left;
    if (nbits_left < 0) {
        r->budget_stop = 1;
        r->stop_pair = 0;
        r->stop_phase = 0;
        nbits_left = 0;
    }

    for (int i = 0; i < ne; i++)
        r->xr[i] = (float)r->xq[i];

    if (!lsb_mode) {
        /* Residual correction, non-HR constants.  Mirrors get_residual:
         * zeros are skipped without spending a bit, the counter only
         * moves on coefficients actually read. */
        const float off1 = 5.f / 16.f;
        const float off2 = 3.f / 16.f;
        int nbits = nbits_left;

        for (int i = 0; i < nq && nbits > 0; i++) {
            if (r->xr[i] == 0.f)
                continue;
            int bit = er_get_bit(pb_h);
            nbits--;
            r->residual_applied++;
            r->pairs[i >> 1].defer_bits++;
            if (bit == 0)
                r->xr[i] -= r->xr[i] < 0.f ? off1 : off2;
            else
                r->xr[i] += r->xr[i] > 0.f ? off1 : off2;
        }
        if (nbits_left > 0 && nbits <= 0) {
            r->budget_stop = 1;
            r->stop_phase = 1;
            r->stop_pair = nq;
        }
    } else {
        /* Deferred LSB recovery, strict mirror of get_lsb() including the
         * way the local counter is decremented before each guarded read.
         * When the budget runs out inside a pair, the remaining member is
         * left untouched rather than reconstructed from padding zeroes. */
        int nbits = nbits_left;
        for (int p = 0; p < nq && nbits > 0; p += 2) {
            float a = std::fabs(r->xr[p + 0]);
            float b = std::fabs(r->xr[p + 1]);
            if (std::fmax(a, b) < 4.f)
                continue;

            r->lsb_pairs++;
            int spent_here = 0;

            if (nbits-- > 0 && er_get_bit(pb_h)) {
                spent_here++;
                if (a != 0.f) {
                    r->xr[p + 0] += r->xr[p + 0] < 0.f ? -1.f : 1.f;
                    seed = (uint16_t)((seed + p) & 0xffff);
                } else if (nbits-- > 0) {
                    int sign = er_get_bit(pb_h);
                    spent_here++;
                    r->xr[p + 0] = sign ? -1.f : 1.f;
                    if (sign) r->lsb_unit_neg++; else r->lsb_unit_pos++;
                    seed = (uint16_t)((seed + p) & 0xffff);
                } else {
                    /* Budget died between the marker bit and the sign. */
                    r->budget_stop = 1;
                    r->stop_pair = p;
                    r->stop_phase = 2;
                }
            }

            if (nbits-- > 0 && er_get_bit(pb_h)) {
                spent_here++;
                if (b != 0.f) {
                    r->xr[p + 1] += r->xr[p + 1] < 0.f ? -1.f : 1.f;
                    seed = (uint16_t)((seed + p + 1) & 0xffff);
                } else if (nbits-- > 0) {
                    int sign = er_get_bit(pb_h);
                    spent_here++;
                    r->xr[p + 1] = sign ? -1.f : 1.f;
                    if (sign) r->lsb_unit_neg++; else r->lsb_unit_pos++;
                    seed = (uint16_t)((seed + p + 1) & 0xffff);
                } else {
                    r->budget_stop = 1;
                    r->stop_pair = p;
                    r->stop_phase = 2;
                }
            }
            r->pairs[p >> 1].defer_bits += spent_here;

            if (nbits <= 0 && (p + 2) < nq) {
                r->budget_stop = 1;
                if (r->stop_pair < 0) {
                    r->stop_pair = p + 2;
                    r->stop_phase = 0;
                }
            }
        }
    }

    r->nf_seed = seed;

    /* Gain unquantization, double precision power as required. */
    int g_int = g_idx - er_gain_offset(sr, nbytes);
    double g = std::pow(10.0, (double)g_int / 28.0);

    for (int i = 0; i < nq; i++)
        r->xg[i] = (float)((double)r->xr[i] * g);
    for (int i = nq; i < ne; i++)
        r->xg[i] = 0.f;

    /* Noise filling. */
    bool fill_active = nq > 2 || r->xq[0] != 0 || r->xq[1] != 0 ||
                       g_idx > 0 || nf < 7;
    if (fill_active) {
        int bw_stop = ne;
        int w = 1 + (dt >= LC3_DT_7M5) + (dt >= LC3_DT_10M);
        float s = (float)g * (float)(8 - nf) / 16.f;
        int z = 0;
        uint16_t nseed = seed;

        int i = 6 * (1 + dt) - w;
        for ( ; i < (nq < bw_stop ? nq : bw_stop); i++) {
            z = r->xq[i] ? 0 : z + 1;
            if (z > 2 * w) {
                nseed = (uint16_t)((13849 + nseed * 31821) & 0xffff);
                r->xg[i - w] = (nseed & 0x8000) ? -s : s;
            }
        }
        for ( ; i < bw_stop + w; i++) {
            if (++z > 2 * w) {
                nseed = (uint16_t)((13849 + nseed * 31821) & 0xffff);
                r->xg[i - w] = (nseed & 0x8000) ? -s : s;
            }
        }
    }

    r->bits_left_at_end = 8 * nbytes -
        er_ac_dec_bits_used(ac_h) - er_bits_used(pb_h);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Independent spectral quantized writer                              */
/* ------------------------------------------------------------------ */

struct er_write_plan {
    int ne, nq;
    int lsb_mode, high_rate;
    int xq[ER_MAX_NE];      /* signed integer quantized coefficients */
    float xg[ER_MAX_NE];    /* scaled values, for residual bits */
};

extern "C" {

struct er_write_handle { er_write_plan p; };

er_write_handle *er_write_open(void) { return new er_write_handle; }
void er_write_close(er_write_handle *h) { delete h; }
er_write_plan *er_write_get_plan(er_write_handle *h) { return &h->p; }

} /* extern C */

/* Mirrors put_quantized() with the standalone cursors only. */
extern "C" int er_spec_write(er_write_handle *wh,
    er_ac_enc_handle *ac_h, er_bits_handle *pb_h, int dt, int sr)
{
    (void)dt; (void)sr;   /* geometry already carried by the plan */
    er_write_plan *p = &wh->p;
    const int ne = p->ne, nq = p->nq, lsb_mode = p->lsb_mode;
    const int high_rate = p->high_rate;

    uint8_t state = 0;

    for (int h = 0, i = 0; h < 2; h++) {
        const uint8_t (*lut_coeff)[4] = lc3_spectrum_lookup[high_rate][h];
        int limit = (nq < (ne + 2) >> (1 - h)) ? nq : (ne + 2) >> (1 - h);

        for ( ; i < limit; i += 2) {
            const float xq_off = 6.f / 16.f;
            uint32_t a = (uint32_t)std::fabs((float)p->xq[i + 0]) +
                         (uint32_t)xq_off;
            uint32_t b = (uint32_t)std::fabs((float)p->xq[i + 1]) +
                         (uint32_t)xq_off;
            const uint8_t *lut = lut_coeff[state];

            uint32_t m = (a | b) >> 2;
            unsigned k = 0, shr = 0;

            if (m) {
                if (lsb_mode)
                    er_ac_enc_symbol(ac_h,
                        lc3_spectrum_models + lut[k++], 16);

                for (m >>= lsb_mode; m; m >>= 1, k++) {
                    er_put_bit(pb_h, (a >> k) & 1);
                    er_put_bit(pb_h, (b >> k) & 1);
                    er_ac_enc_symbol(ac_h,
                        lc3_spectrum_models + lut[LC3_MIN(k, 3u)], 16);
                }

                a >>= lsb_mode;
                b >>= lsb_mode;
                shr = k - lsb_mode;
                k = LC3_MIN(k, 3u);
            }

            if (a) er_put_bit(pb_h, p->xq[i + 0] < 0);
            if (b) er_put_bit(pb_h, p->xq[i + 1] < 0);

            a >>= shr;
            b >>= shr;

            er_ac_enc_symbol(ac_h,
                lc3_spectrum_models + lut[k], (int)(a + 4 * b));

            state = (uint8_t)((state << 4) +
                (k > 1 ? 12 + k : 1 + (a + b) * (k + 1)));
        }
    }
    return 0;
}

/* Independent residual writer, mirror of put_residual() non-HR path. */
extern "C" int er_residual_write(er_write_handle *wh,
    er_bits_handle *pb_h, int nbits)
{
    er_write_plan *p = &wh->p;
    const float xq_lim = 10.f / 16.f;
    const float xq_off0 = xq_lim / 2.f;
    float xq_off = xq_off0;

    /* Work on a private copy so the caller's scaled vector is preserved. */
    float x[ER_MAX_NE];
    for (int i = 0; i < p->nq; i++)
        x[i] = p->xg[i];

    for (int iter = 0; iter < 1 && nbits > 0; iter++) {
        for (int i = 0; i < p->nq && nbits > 0; i++) {
            float xr = std::fabs(x[i]);
            if (xr < xq_lim)
                continue;
            int bit = ((xr - std::trunc(xr) < xq_lim) ^ (x[i] < 0));
            er_put_bit(pb_h, bit);
            nbits--;
            x[i] += bit ? -xq_off : xq_off;
        }
        xq_off *= xq_lim;
    }
    return 0;
}

/* Count of residual plain bits the plan would emit with no budget cap. */
extern "C" int er_residual_demand(er_write_handle *wh)
{
    er_write_plan *p = &wh->p;
    const float xq_lim = 10.f / 16.f;
    int n = 0;
    for (int i = 0; i < p->nq; i++)
        if (std::fabs(p->xg[i]) >= xq_lim)
            n++;
    return n;
}

/* Count of deferred LSB plain bits the plan would emit uncapped. */
extern "C" int er_lsb_demand(er_write_handle *wh)
{
    er_write_plan *p = &wh->p;
    const float xq_off = 6.f / 16.f;
    int n = 0;
    for (int i = 0; i < p->nq; i += 2) {
        uint32_t a = (uint32_t)std::fabs((float)p->xq[i + 0]) +
                     (uint32_t)xq_off;
        uint32_t b = (uint32_t)std::fabs((float)p->xq[i + 1]) +
                     (uint32_t)xq_off;
        if ((a | b) >> 2 == 0)
            continue;
        n += 2;
        if (a == 1) n++;
        if (b == 1) n++;
    }
    return n;
}

/* Independent deferred LSB writer, mirror of put_lsb() non-HR path. */
extern "C" int er_lsb_write(er_write_handle *wh,
    er_bits_handle *pb_h, int nbits)
{
    er_write_plan *p = &wh->p;

    for (int i = 0; i < p->nq && nbits > 0; i += 2) {
        const float xq_off = 6.f / 16.f;
        uint32_t a = (uint32_t)std::fabs((float)p->xq[i + 0]) +
                     (uint32_t)xq_off;
        uint32_t b = (uint32_t)std::fabs((float)p->xq[i + 1]) +
                     (uint32_t)xq_off;

        if ((a | b) >> 2 == 0)
            continue;

        if (nbits-- > 0)
            er_put_bit(pb_h, a & 1);
        if (a == 1 && nbits-- > 0)
            er_put_bit(pb_h, p->xq[i + 0] < 0);
        if (nbits-- > 0)
            er_put_bit(pb_h, b & 1);
        if (b == 1 && nbits-- > 0)
            er_put_bit(pb_h, p->xq[i + 1] < 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Whole frame reference parsing                                      */
/* ------------------------------------------------------------------ */

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

extern "C" {

struct er_frame_handle { er_frame_result r; };

er_frame_handle *er_frame_open(void) { return new er_frame_handle; }
void er_frame_close(er_frame_handle *h) { delete h; }
const er_frame_result *er_frame_get(const er_frame_handle *h) { return &h->r; }

} /* extern C */

/* Advance the standalone readers through the side information and the
 * TNS arithmetic prefix, which shares the interval state with the
 * spectrum section.  Everything is walked from byte 0 with the reference
 * cursors only. */
extern "C" int er_frame_parse(er_ac_dec_handle *ac_h, er_bits_handle *pb_h,
    const er_frame_side *plan, er_frame_handle *out_h,
    char *err, int errlen)
{
    er_frame_result *r = &out_h->r;
    r->side = *plan;
    r->error = 0;

    const int dt = plan->dt, sr = plan->sr, nbytes = plan->nbytes;
    const int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    /* Bandwidth indication, plain backward bits. */
    int nbits_bw = (sr > 0) + (sr > 1) + (sr > 3);
    int bw_read = nbits_bw ? (int)er_get_bits(pb_h, nbits_bw) : sr;
    if (bw_read != plan->bw) {
        if (err) std::snprintf(err, errlen, "bw mismatch");
        return -1;
    }

    /* Spectrum side: nq, lsb mode, gain. */
    int nbits_nq = er_nbits_nq(ne);
    int nq_read = ((int)er_get_bits(pb_h, nbits_nq) + 1) << 1;
    int lsb_read = er_get_bit(pb_h);
    int g_read = (int)er_get_bits(pb_h, 8);
    if (nq_read != plan->nq || lsb_read != plan->lsb_mode ||
        g_read != plan->g_idx) {
        if (err) std::snprintf(err, errlen, "spec side mismatch");
        return -1;
    }

    /* TNS data: one plain presence bit per filter, then AC symbols that
     * advance the very same interval coder as the spectrum. */
    const er_tns_plan *tp = &plan->tns;
    for (int f = 0; f < tp->nfilters; f++) {
        int present = er_get_bit(pb_h);
        if (present != (tp->order[f] > 0)) {
            if (err) std::snprintf(err, errlen, "tns presence mismatch");
            return -1;
        }
        if (!present)
            continue;

        int order = er_ac_dec_symbol(ac_h,
            lc3_tns_order_models + tp->lpc_weighting) + 1;
        if (order != tp->order[f]) {
            if (err) std::snprintf(err, errlen, "tns order mismatch");
            return -1;
        }
        for (int i = 0; i < order; i++) {
            int rc = er_ac_dec_symbol(ac_h,
                lc3_tns_coeffs_models + i) - 8;
            if (rc != tp->rc[f][i]) {
                if (err) std::snprintf(err, errlen, "tns rc mismatch");
                return -1;
            }
        }
    }
    r->ac_after_tns_pos = er_ac_dec_pos(ac_h);

    /* Pitch present flag, then the 38 SNS plain bits. */
    int pitch = er_get_bit(pb_h);
    if (pitch != plan->pitch_present) {
        if (err) std::snprintf(err, errlen, "pitch flag mismatch");
        return -1;
    }
    er_get_bits(pb_h, 38);
    r->plain_after_sns = er_bits_used(pb_h);

    /* Noise factor, then the spectrum section. */
    int nf = (int)er_get_bits(pb_h, 3);
    if (nf != plan->nf) {
        if (err) std::snprintf(err, errlen, "noise factor mismatch");
        return -1;
    }

    auto *sh = er_spec_open();
    int rc = er_spec_decode(ac_h, pb_h, dt, sr, nbytes,
        plan->nq, plan->lsb_mode, nf, plan->g_idx, sh, err, errlen);
    r->spec = *er_spec_get(sh);
    er_spec_close(sh);
    return rc;
}
