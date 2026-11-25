/*
 * Production static-stage test bridge, compiled as plain C.
 *
 * It #includes the real ../src/spec.c so the file-local entropy stages
 * (get_quantized, get_residual, get_lsb, estimate_noise, fill_noise and
 * the interval/bit-plane machinery they use) can be driven directly over
 * raw packet bytes. The public lc3_spec_decode() is renamed at inclusion
 * time and re-exported through a thin wrapper, which lets the
 * LC3_ENTROPY_TRACE build keep a read-only snapshot of the spectrum
 * immediately after spectral decode and before TNS synthesis, without
 * touching the production source. No state, callback or allocation is
 * added to a normal (non-trace) build.
 *
 * Expected values are always supplied by the C++ independent reference;
 * nothing here computes an expectation.
 */

#include "target_bridge.h"

#include <string.h>

/* Rename the public entry so the bridge can wrap it. The rest of the
 * library keeps linking against the re-exported name below. */
#define lc3_spec_decode lc3_spec_decode_impl
#include "../src/spec.c"
#undef lc3_spec_decode

/* ------------------------------------------------------------------ */
/* Format data accessors                                               */
/* ------------------------------------------------------------------ */

uint8_t bridge_lut(int high_rate, int half, int state, int k)
{
    return lc3_spectrum_lookup[high_rate][half][state][k];
}

const void* bridge_spectrum_model(int index)
{
    return &lc3_spectrum_models[index];
}

const void* bridge_tns_order_model(int index)
{
    return &lc3_tns_order_models[index];
}

const void* bridge_tns_coeff_model(int index)
{
    return &lc3_tns_coeffs_models[index];
}

/* ------------------------------------------------------------------ */
/* Small formulas mirrored for cross-checking (not expectations)       */
/* ------------------------------------------------------------------ */

int bridge_resolve_modes(int sr, int nbytes, int* lsb_mode)
{
    bool lsb = false;
    bool high = resolve_modes((enum lc3_srate)sr, nbytes, &lsb);
    if (lsb_mode)
        *lsb_mode = (int)lsb;
    return (int)high;
}

int bridge_gain_offset(int sr, int nbytes)
{
    return resolve_gain_offset((enum lc3_srate)sr, nbytes);
}

float bridge_unquantize_gain(int g_int)
{
    return unquantize_gain(g_int);
}

/* ------------------------------------------------------------------ */
/* Position a read stream just before the spectral section.           */
/* Mirrors the side-data consumption order of lc3.c decode().         */
/* ------------------------------------------------------------------ */

static int open_stream_after_header(
    const uint8_t* data, int nbytes,
    int dt, int sr,
    lc3_bits_t* bits, lc3_spec_side_t* side,
    struct lc3_tns_data* tns)
{
    enum lc3_bandwidth bw;
    int ret;

    lc3_setup_bits(bits, LC3_BITS_MODE_READ, (void*)data, nbytes);

    if ((ret = lc3_bwdet_get_bw(bits, (enum lc3_srate)sr, &bw)) < 0)
        return ret;

    if ((ret = lc3_spec_get_side(bits,
                    (enum lc3_dt)dt, (enum lc3_srate)sr, side)) < 0)
        return ret;

    if ((ret = lc3_tns_get_data(bits, (enum lc3_dt)dt,
                    bw, nbytes, tns)) < 0)
        return ret;

    (void)lc3_get_bit(bits);                 /* pitch present */

    struct lc3_sns_data sns;
    if ((ret = lc3_sns_get_data(bits, &sns)) < 0)
        return ret;

    return 0;
}

int bridge_estimate_noise(int dt, int bw, const float* x, int nq)
{
    return estimate_noise((enum lc3_dt)dt, (enum lc3_bandwidth)bw,
                          false, x, nq);
}

/* Single-stream run of every production spectral decode stage. The
 * bitstream context opened for the header is carried unchanged through
 * get_quantized and get_residual/get_lsb, so each stage starts at the
 * exact cursor position the previous one left. Snapshots are taken at
 * each stage boundary for the independent reference comparison. */
void bridge_spec_stages(const uint8_t* data, int nbytes,
    int dt, int sr, int bw, int nq, int lsb_mode, int g_idx,
    bridge_stages_t* out)
{
    lc3_bits_t bits;
    lc3_spec_side_t side;
    struct lc3_tns_data tns;
    memset(out, 0, sizeof(*out));

    int hret = open_stream_after_header(
        data, nbytes, dt, sr, &bits, &side, &tns);
    if (hret < 0) {
        out->ret = hret;
        out->error = 1;
        return;
    }

    int ne = lc3_ne((enum lc3_dt)dt, (enum lc3_srate)sr);

    /* Noise factor. */
    out->nf = get_noise_factor(&bits);

    /* Quantized integer coefficients. */
    uint16_t nf_seed = 0;
    int ret = get_quantized(&bits, (enum lc3_dt)dt, (enum lc3_srate)sr,
        nbytes, nq, lsb_mode, out->xq, &nf_seed);
    out->nf_seed_quant = nf_seed;
    out->bits_left_after_quant = lc3_get_bits_left(&bits);
    if (ret < 0) {
        out->ret = ret;
        out->error = 1;
        return;
    }
    memcpy(out->xref, out->xq, (size_t)ne * sizeof(float));

    /* Residual (LSB off) or deferred LSB plane, on the same stream. */
    int nbits_left = lc3_get_bits_left(&bits);
    if (lsb_mode)
        get_lsb(&bits, nbits_left, out->xref, nq, &nf_seed);
    else
        get_residual(&bits, nbits_left, false, out->xref, nq);
    out->nf_seed_final = nf_seed;
    memcpy(out->xfinal, out->xref, (size_t)ne * sizeof(float));

    /* Gain and noise filling, mirroring lc3_spec_decode. */
    int g_int = g_idx -
        resolve_gain_offset((enum lc3_srate)sr, nbytes);
    float g = unquantize((enum lc3_dt)dt, (enum lc3_srate)sr,
        g_int, out->xfinal, nq);

    if (nq > 2 || out->xq[0] || out->xq[1] || g_idx > 0 || out->nf < 7)
        fill_noise((enum lc3_dt)dt, (enum lc3_bandwidth)bw,
            out->nf, nf_seed, g, out->xfinal, nq);

    out->ret = 0;
    out->error = (int)lc3_check_bits(&bits);
}

/* ------------------------------------------------------------------ */
/* Production bit-plane writer/reader driven by an explicit op plan    */
/* ------------------------------------------------------------------ */

void bridge_prod_write(int nbytes,
    const bridge_op_t* ops, int nops, uint8_t* out)
{
    lc3_bits_t bits;
    lc3_setup_bits(&bits, LC3_BITS_MODE_WRITE, out, nbytes);

    for (int i = 0; i < nops; i++) {
        switch (ops[i].kind) {
        case BRIDGE_OP_SYMBOL:
            lc3_put_symbol(&bits,
                lc3_spectrum_models + ops[i].model, ops[i].sym);
            break;
        case BRIDGE_OP_BIT:
            lc3_put_bit(&bits, (int)ops[i].value);
            break;
        case BRIDGE_OP_BITS:
            lc3_put_bits(&bits, ops[i].value, ops[i].nbits);
            break;
        default:
            break;
        }
    }

    lc3_flush_bits(&bits);
}

void bridge_prod_read(const uint8_t* data, int nbytes,
    const bridge_op_t* ops, int nops, bridge_plane_t* out)
{
    lc3_bits_t bits;
    memset(out, 0, sizeof(*out));
    lc3_setup_bits(&bits, LC3_BITS_MODE_READ, (void*)data, nbytes);

    for (int i = 0; i < nops && i < 256; i++) {
        switch (ops[i].kind) {
        case BRIDGE_OP_SYMBOL: {
            unsigned s = lc3_get_symbol(&bits,
                lc3_spectrum_models + ops[i].model);
            out->symbols[i] = (int)s;
            break;
        }
        case BRIDGE_OP_BIT:
            out->value[i] = (unsigned)lc3_get_bit(&bits);
            break;
        case BRIDGE_OP_BITS:
            out->value[i] = lc3_get_bits(&bits, ops[i].nbits);
            break;
        default:
            break;
        }
        out->nops = i + 1;
    }

    out->bits_left = lc3_get_bits_left(&bits);
    out->error = (int)lc3_check_bits(&bits);
}

/* ------------------------------------------------------------------ */
/* Public re-export with the trace observation point                   */
/* ------------------------------------------------------------------ */

int lc3_spec_decode(lc3_bits_t* bits,
    enum lc3_dt dt, enum lc3_srate sr, enum lc3_bandwidth bw,
    int nbytes, const lc3_spec_side_t* side, float* x)
{
    int ret = lc3_spec_decode_impl(bits, dt, sr, bw, nbytes, side, x);

#ifdef LC3_ENTROPY_TRACE
    if (ret == 0)
        bridge_capture_spectrum(x, lc3_ne(dt, sr));
#endif

    return ret;
}

#ifdef LC3_ENTROPY_TRACE

#define TRACE_CAP 400

static float g_trace_x[TRACE_CAP];
static int g_trace_ne;
static int g_trace_frames;

void bridge_capture_spectrum(const float* x, int ne)
{
    if (ne > TRACE_CAP)
        ne = TRACE_CAP;
    memcpy(g_trace_x, x, (size_t)ne * sizeof(float));
    g_trace_ne = ne;
    g_trace_frames++;
}

int bridge_trace_copy(float* dst, int cap)
{
    int n = g_trace_ne < cap ? g_trace_ne : cap;
    memcpy(dst, g_trace_x, (size_t)n * sizeof(float));
    return n;
}

void bridge_trace_reset(void)
{
    memset(g_trace_x, 0, sizeof(g_trace_x));
    g_trace_ne = 0;
    g_trace_frames = 0;
}

int bridge_trace_frames(void)
{
    return g_trace_frames;
}

#else  /* !LC3_ENTROPY_TRACE: no state, callback or allocation. */

void bridge_capture_spectrum(const float* x, int ne) { (void)x; (void)ne; }
int bridge_trace_copy(float* dst, int cap) { (void)dst; (void)cap; return 0; }
void bridge_trace_reset(void) {}
int bridge_trace_frames(void) { return 0; }

#endif
