/*
 * Test bridge over the production spectral stage.
 *
 * This translation unit is compiled as plain C and physically includes
 * src/spec.c, so the file-local stages of the production implementation
 * (mode resolution, quantized coefficient reading, residual and LSB
 * recovery, noise handling) can be driven individually by the tests.
 *
 * The expectations compared against these bridges are always produced by
 * the independent C++ reference in entropy_reference.cpp; the bridge only
 * exposes what the code under test actually computes.
 *
 * When LC3_ENTROPY_TRACE is defined, lc3_spec_decode() additionally leaves
 * a read-only snapshot of the spectrum as it is right after the spectral
 * decode and before TNS synthesis.  The macro is the only thing that adds
 * state, and the normal library build defines nothing of the kind.
 */

#include "../src/spec.c"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Static stage bridges                                               */
/* ------------------------------------------------------------------ */

int bridge_resolve_modes(int sr, int nbytes, int *lsb_mode, int *high_rate)
{
    bool lsb = false;
    bool hr = resolve_modes((enum lc3_srate)sr, nbytes, &lsb);
    if (lsb_mode) *lsb_mode = (int)lsb;
    if (high_rate) *high_rate = (int)hr;
    return (int)hr;
}

int bridge_gain_offset(int sr, int nbytes)
{
    return resolve_gain_offset((enum lc3_srate)sr, nbytes);
}

int bridge_get_side(lc3_bits_t *bits, int dt, int sr,
    lc3_spec_side_t *side)
{
    return lc3_spec_get_side(bits,
        (enum lc3_dt)dt, (enum lc3_srate)sr, side);
}

int bridge_get_quantized(lc3_bits_t *bits, int dt, int sr, int nbytes,
    int nq, int lsb_mode, float *x, uint16_t *nf_seed)
{
    return get_quantized(bits, (enum lc3_dt)dt, (enum lc3_srate)sr,
        nbytes, nq, (bool)lsb_mode, x, nf_seed);
}

void bridge_get_residual(lc3_bits_t *bits, int nbits, float *x, int n)
{
    get_residual(bits, nbits, false, x, n);
}

void bridge_get_lsb(lc3_bits_t *bits, int nbits, float *x, int nq,
    uint16_t *nf_seed)
{
    get_lsb(bits, nbits, x, nq, nf_seed);
}

float bridge_unquantize_gain(int g_int)
{
    return unquantize_gain(g_int);
}

int bridge_estimate_noise(int dt, int bw, const float *x, int n)
{
    return estimate_noise((enum lc3_dt)dt,
        (enum lc3_bandwidth)bw, false, x, n);
}

void bridge_fill_noise(int dt, int bw, int nf, uint16_t nf_seed,
    float g, float *x, int nq)
{
    fill_noise((enum lc3_dt)dt, (enum lc3_bandwidth)bw,
        nf, nf_seed, g, x, nq);
}

int bridge_spec_decode(lc3_bits_t *bits, int dt, int sr, int bw,
    int nbytes, const lc3_spec_side_t *side, float *x)
{
    return lc3_spec_decode(bits, (enum lc3_dt)dt, (enum lc3_srate)sr,
        (enum lc3_bandwidth)bw, nbytes, side, x);
}

int bridge_unquantize(int dt, int sr, int g_int, float *x, int nq)
{
    return (int)unquantize((enum lc3_dt)dt, (enum lc3_srate)sr,
        g_int, x, nq);
}

/* Production serializers, only used to turn an explicit coefficient plan
 * into a real stream.  The expected decoded values always come from the
 * plan and the independent reference, never from these functions. */

void bridge_put_quantized(lc3_bits_t *bits, int dt, int sr, int nbytes,
    const float *x, int nq, int lsb_mode)
{
    put_quantized(bits, (enum lc3_dt)dt, (enum lc3_srate)sr, nbytes,
        x, nq, (bool)lsb_mode);
}

void bridge_put_residual(lc3_bits_t *bits, int nbits, float *x, int n)
{
    put_residual(bits, nbits, false, x, n);
}

void bridge_put_lsb(lc3_bits_t *bits, int nbits, const float *x, int n)
{
    put_lsb(bits, nbits, false, x, n);
}

void bridge_spec_put_side(lc3_bits_t *bits, int dt, int sr,
    const lc3_spec_side_t *side)
{
    lc3_spec_put_side(bits, (enum lc3_dt)dt, (enum lc3_srate)sr, side);
}

int bridge_get_bits_left(const lc3_bits_t *bits)
{
    return lc3_get_bits_left(bits);
}

int bridge_check_bits(const lc3_bits_t *bits)
{
    return lc3_check_bits(bits);
}

/* ------------------------------------------------------------------ */
/* Read-only decode trace, test builds only                           */
/* ------------------------------------------------------------------ */

#ifdef LC3_ENTROPY_TRACE

#define BRIDGE_TRACE_SLOTS 8

struct bridge_trace_slot {
    int valid;
    int dt, sr, nq;
    float x[LC3_MAX_NE];
};

static struct bridge_trace_slot bridge_trace[BRIDGE_TRACE_SLOTS];
static int bridge_trace_index;
static int bridge_trace_total;

/* Observation hook called from lc3_spec_decode() itself, so the snapshot
 * is taken on the real lc3_decode() path, after spectral decode and noise
 * filling, before TNS synthesis.  The function only reads the spectrum. */
void lc3_entropy_trace_hook(enum lc3_dt dt, enum lc3_srate sr,
    const lc3_spec_side_t *side, const float *x)
{
    struct bridge_trace_slot *s =
        &bridge_trace[bridge_trace_index % BRIDGE_TRACE_SLOTS];
    int ne = lc3_ne(dt, sr);
    s->valid = 1;
    s->dt = (int)dt;
    s->sr = (int)sr;
    s->nq = side->nq < ne ? side->nq : ne;
    memcpy(s->x, x, (size_t)ne * sizeof(float));
    bridge_trace_index++;
    bridge_trace_total++;
}

void bridge_trace_reset(void)
{
    bridge_trace_index = 0;
    bridge_trace_total = 0;
    memset(bridge_trace, 0, sizeof(bridge_trace));
}

int bridge_trace_count(void)
{
    return bridge_trace_total;
}

/* Copy the snapshot at ordinal `frame` (0 based, relative to the last
 * reset) into dst.  Returns 0 on success, -1 if the slot aged out. */
int bridge_trace_copy(int frame, float *dst, int n,
    int *dt, int *sr, int *nq)
{
    int age = bridge_trace_total - 1 - frame;
    if (age < 0 || age >= BRIDGE_TRACE_SLOTS || age >= bridge_trace_total)
        return -1;

    int idx = (bridge_trace_index - 1 - age) % BRIDGE_TRACE_SLOTS;
    const struct bridge_trace_slot *s = &bridge_trace[idx];
    if (!s->valid)
        return -1;

    memcpy(dst, s->x, (size_t)n * sizeof(float));
    if (dt) *dt = s->dt;
    if (sr) *sr = s->sr;
    if (nq) *nq = s->nq;
    return 0;
}

#else

void bridge_trace_reset(void) {}
int bridge_trace_count(void) { return 0; }
int bridge_trace_copy(int, float *, int, int *, int *, int *) { return -1; }

#endif
