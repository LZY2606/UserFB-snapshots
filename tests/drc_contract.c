/*
** Contract tests for the AAC dynamic range (DRC) path.
**
** Covers three groups:
**  - gain:     drc_decode_frame() gain math, band layout, validation,
**              saturation (checked against an independent double formula)
**  - metadata: consecutive EXT_DYNAMIC_RANGE payloads parsed through the
**              real bit reading path reset to defaults when fields are
**              omitted
**  - stream:   end-to-end NeAACDecInit2/NeAACDecDecode runs of generated
**              AAC-LC frames with nonzero spectrum, 960/1024 frame
**              lengths, mono/stereo and excluded channels
**
** The file exports task_cases/task_case_count for task_support/driver.c;
** it has no main() of its own. Test streams are generated in C below.
*/

#include "task_support/check.h"

#include "common.h"
#include "structs.h"
#include "drc.h"
#include "syntax.h"
#include "bits.h"

#include "codebook/hcb.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static double spec_to_double(real_t x)
{
#ifdef FIXED_POINT
    return (double)x / (double)REAL_PRECISION;
#else
    return (double)x;
#endif
}

/* compare a decoded spectral value against the independent double
   precision formula; fixed point allows 2 Q14 integer units, floating
   point 2e-5 relative */
static int gain_matches(double got, double expected)
{
#ifdef FIXED_POINT
    return fabs(got * (double)REAL_PRECISION - expected * (double)REAL_PRECISION) <= 2.0;
#else
    if (expected == 0.0)
        return got == 0.0;
    return fabs(got / expected - 1.0) <= 2e-5;
#endif
}

/* independent reference formula for the DRC gain */
static double ref_gain(double ctrl, int sgn, int ctl, int ref)
{
    double e;
    if (sgn) /* compress */
        e = (-ctrl * ctl - (DRC_REF_LEVEL - ref)) / 24.0;
    else /* boost */
        e = (ctrl * ctl - (DRC_REF_LEVEL - ref)) / 24.0;
    return pow(2.0, e);
}

/* ------------------------------------------------------------------ */
/* bit writer                                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t buf[2048];
    uint32_t nbits;
} bitw;

static void bw_init(bitw *w)
{
    w->nbits = 0;
}

static void bw_put(bitw *w, uint32_t v, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++)
    {
        uint32_t bit = (v >> (n - 1 - i)) & 1u;
        uint32_t pos = w->nbits++;
        w->buf[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7));
        if (bit)
            w->buf[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
    }
}

static uint32_t bw_len(const bitw *w)
{
    return (w->nbits + 7) / 8;
}

/* ------------------------------------------------------------------ */
/* Huffman encoders, inverted at runtime from the decoder tables       */
/* ------------------------------------------------------------------ */

static uint32_t cb1_code[81]; /* signed quad (values -1..1) -> codeword */
static uint8_t  cb1_len[81];
static uint8_t  cb1_ok[81];
static uint32_t sf_code[121];
static uint8_t  sf_len[121];
static int      tables_ready = 0;

static unsigned cb1_key(int x, int y, int v, int z)
{
    return (unsigned)((x + 1) * 27 + (y + 1) * 9 + (v + 1) * 3 + (z + 1));
}

static void sf_walk(uint16_t off, uint32_t code, uint8_t len)
{
    if (hcb_sf[off][1] == 0)
    {
        sf_code[hcb_sf[off][0]] = code;
        sf_len[hcb_sf[off][0]] = len;
        return;
    }
    sf_walk((uint16_t)(off + hcb_sf[off][0]), code << 1, (uint8_t)(len + 1));
    sf_walk((uint16_t)(off + hcb_sf[off][1]), (code << 1) | 1u, (uint8_t)(len + 1));
}

static void build_tables(void)
{
    uint32_t cw5, e;

    if (tables_ready)
        return;

    /* invert the 2-step codebook 1 table; codebook 1 folds the sign
       into the codeword, no separate sign bits follow */
    for (cw5 = 0; cw5 < 32; cw5++)
    {
        uint8_t off = hcb1_1[cw5].offset;
        uint8_t eb = hcb1_1[cw5].extra_bits;
        for (e = 0; e < (1u << eb); e++)
        {
            uint8_t idx = (uint8_t)(off + e);
            uint8_t len = hcb1_2[idx].bits;
            uint32_t code = ((cw5 << eb) | e) >> (5 + eb - len);
            unsigned key = cb1_key(hcb1_2[idx].x, hcb1_2[idx].y,
                                   hcb1_2[idx].v, hcb1_2[idx].w);
            if (!cb1_ok[key])
            {
                cb1_ok[key] = 1;
                cb1_code[key] = code;
                cb1_len[key] = len;
            }
        }
    }

    sf_walk(0, 0, 0);

    tables_ready = 1;
}

/* emit one codebook 1 quadruple, values in {-1,0,1} */
static void bw_quad1(bitw *w, int a, int b, int c, int d)
{
    unsigned key = cb1_key(a, b, c, d);
    bw_put(w, cb1_code[key], cb1_len[key]);
}

/* ------------------------------------------------------------------ */
/* AAC-LC frame generation                                             */
/* ------------------------------------------------------------------ */

#define GEN_MAX_SFB 4
#define GEN_GLOBAL_GAIN 120

/* DRC extension description used by the generator */
typedef struct
{
    uint8_t has_tag;
    uint8_t tag;
    uint8_t has_excl;
    uint8_t excl_mask[7];
    uint8_t has_bands;
    uint8_t band_incr;
    uint8_t band_top[17];
    uint8_t has_ref;
    uint8_t ref;
    uint8_t sgn[17];
    uint8_t ctl[17];
} drc_desc;

static void emit_drc_fill(bitw *w, const drc_desc *d)
{
    uint8_t num_bands = (uint8_t)(1 + (d->has_bands ? d->band_incr : 0));
    uint32_t bits = 8; /* extension_type + the four presence bits */
    uint32_t count;
    uint8_t i;

    if (d->has_tag)   bits += 8;
    if (d->has_excl)  bits += 8;
    if (d->has_bands) bits += 8 + 8u * num_bands;
    if (d->has_ref)   bits += 8;
    bits += 8u * num_bands; /* dyn_rng_sgn + dyn_rng_ctl per band */
    count = bits / 8;

    bw_put(w, 6, 3);     /* ID_FIL */
    bw_put(w, count, 4); /* count (small enough to not need escape) */
    bw_put(w, 11, 4);    /* EXT_DYNAMIC_RANGE */

    bw_put(w, d->has_tag, 1);
    if (d->has_tag)
    {
        bw_put(w, d->tag, 4);
        bw_put(w, 0, 4); /* reserved */
    }

    bw_put(w, d->has_excl, 1);
    if (d->has_excl)
    {
        for (i = 0; i < 7; i++)
            bw_put(w, d->excl_mask[i], 1);
        bw_put(w, 0, 1); /* no additional excluded channels */
    }

    bw_put(w, d->has_bands, 1);
    if (d->has_bands)
    {
        bw_put(w, d->band_incr, 4);
        bw_put(w, 0, 4); /* reserved */
        for (i = 0; i < num_bands; i++)
            bw_put(w, d->band_top[i], 8);
    }

    bw_put(w, d->has_ref, 1);
    if (d->has_ref)
    {
        bw_put(w, d->ref, 7);
        bw_put(w, 0, 1); /* reserved */
    }

    for (i = 0; i < num_bands; i++)
    {
        bw_put(w, d->sgn[i], 1);
        bw_put(w, d->ctl[i], 7);
    }
}

static void emit_ics_info(bitw *w)
{
    bw_put(w, 0, 1);          /* ics_reserved_bit */
    bw_put(w, 0, 2);          /* ONLY_LONG_SEQUENCE */
    bw_put(w, 0, 1);          /* window_shape: sine */
    bw_put(w, GEN_MAX_SFB, 6);/* max_sfb */
    bw_put(w, 0, 1);          /* predictor_data_present */
}

/* one individual_channel_stream body starting at global_gain */
static void emit_ics(bitw *w, const int (*q)[4], int nq, int with_ics_info)
{
    int i;

    bw_put(w, GEN_GLOBAL_GAIN, 8); /* global_gain */
    if (with_ics_info)
        emit_ics_info(w);

    /* section_data: a single section with codebook 1 over max_sfb bands */
    bw_put(w, 1, 4);              /* sect_cb = 1 */
    bw_put(w, GEN_MAX_SFB, 5);    /* sect_len_incr */

    /* scale_factor_data: delta 0 for every band (sf == global_gain) */
    for (i = 0; i < GEN_MAX_SFB; i++)
        bw_put(w, sf_code[60], sf_len[60]);

    bw_put(w, 0, 1); /* pulse_data_present */
    bw_put(w, 0, 1); /* tns_data_present */
    bw_put(w, 0, 1); /* gain_control_data_present */

    /* spectral_data */
    for (i = 0; i < nq; i++)
        bw_quad1(w, q[i][0], q[i][1], q[i][2], q[i][3]);
}

/* generate one raw_data_block; returns its length in bytes */
static uint32_t gen_frame(uint8_t *out, int stereo,
                          const int (*q1)[4], const int (*q2)[4], int nq,
                          const drc_desc *dd)
{
    bitw w;

    build_tables();
    bw_init(&w);

    if (!stereo)
    {
        bw_put(&w, 0, 3); /* ID_SCE */
        bw_put(&w, 0, 4); /* element_instance_tag */
        emit_ics(&w, q1, nq, 1);
    } else {
        bw_put(&w, 1, 3); /* ID_CPE */
        bw_put(&w, 0, 4); /* element_instance_tag */
        bw_put(&w, 1, 1); /* common_window */
        emit_ics_info(&w);
        bw_put(&w, 0, 2); /* ms_mask_present */
        emit_ics(&w, q1, nq, 0);
        emit_ics(&w, q2, nq, 0);
    }

    if (dd)
        emit_drc_fill(&w, dd);

    bw_put(&w, 7, 3); /* ID_END */
    while (w.nbits & 7)
        bw_put(&w, 0, 1); /* byte alignment */

    memcpy(out, w.buf, bw_len(&w));
    return bw_len(&w);
}

/* generate a raw_data_block holding only fill elements (metadata tests) */
static uint32_t gen_fill_block(uint8_t *out, const drc_desc *d1, const drc_desc *d2)
{
    bitw w;

    build_tables();
    bw_init(&w);

    if (d1)
        emit_drc_fill(&w, d1);
    if (d2)
        emit_drc_fill(&w, d2);

    bw_put(&w, 7, 3); /* ID_END */
    while (w.nbits & 7)
        bw_put(&w, 0, 1);

    memcpy(out, w.buf, bw_len(&w));
    return bw_len(&w);
}

/* ------------------------------------------------------------------ */
/* pre-synthesis spectrum observation point (DRC_CONTRACT_TEST only)   */
/* ------------------------------------------------------------------ */

#define OBS_MAX 8

static double   g_obs[OBS_MAX][1024];
static uint16_t g_obs_len[OBS_MAX];
static uint8_t  g_obs_ch[OBS_MAX];
static int      g_obs_count;

void drc_contract_observe_spec(const real_t *spec, uint16_t frame_len,
                               uint8_t channel)
{
    uint32_t i;
    if (g_obs_count >= OBS_MAX)
        return;
    for (i = 0; i < frame_len; i++)
        g_obs[g_obs_count][i] = spec_to_double(spec[i]);
    g_obs_len[g_obs_count] = frame_len;
    g_obs_ch[g_obs_count] = channel;
    g_obs_count++;
}

static void obs_reset(void)
{
    g_obs_count = 0;
}

/* ------------------------------------------------------------------ */
/* shared test material                                                */
/* ------------------------------------------------------------------ */

/* 4 quadruples = 16 nonzero spectral lines (max_sfb 4 at 44.1 kHz) */
static const int Q_POS[4][4] = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
};
static const int Q_NEG[4][4] = {
    { 0, 0, 0, -1 }, { 0, 0, -1, 0 }, { 0, -1, 0, 0 }, { -1, 0, 0, 0 }
};

/* AudioSpecificConfig: AAC LC, 44.1 kHz */
static const uint8_t ASC_MONO_1024[2]   = { 0x12, 0x08 };
static const uint8_t ASC_MONO_960[2]    = { 0x12, 0x0C };
static const uint8_t ASC_STEREO_1024[2] = { 0x12, 0x10 };
static const uint8_t ASC_STEREO_960[2]  = { 0x12, 0x14 };

static int open_decoder(const uint8_t *asc, NeAACDecHandle *ph,
                        unsigned long *sr, unsigned char *ch)
{
    NeAACDecHandle h = NeAACDecOpen();
    if (!h)
        return 0;
    if (NeAACDecInit2(h, (unsigned char*)asc, 2, sr, ch) != 0)
    {
        NeAACDecClose(h);
        return 0;
    }
    *ph = h;
    return 1;
}

/* ------------------------------------------------------------------ */
/* gain group                                                          */
/* ------------------------------------------------------------------ */

static void fill_spec(real_t *spec, int n, double v)
{
    int i;
    for (i = 0; i < n; i++)
        spec[i] = REAL_CONST(v);
}

static int gain_boost_compress(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];
    int i;

    TASK_CHECK(d != NULL);
    TASK_CHECK(d->prog_ref_level == DRC_REF_LEVEL);

    /* boost, ctl 24 -> gain exactly 2 over the whole 1024 frame */
    d->num_bands = 1;
    d->dyn_rng_sgn[0] = 0;
    d->dyn_rng_ctl[0] = 24;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(spec_to_double(spec[0]) == 2.0);
    TASK_CHECK(spec_to_double(spec[1023]) == 2.0);

    /* compress, ctl 24 -> gain exactly 0.5 */
    d->dyn_rng_sgn[0] = 1;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(spec_to_double(spec[0]) == 0.5);
    TASK_CHECK(spec_to_double(spec[1023]) == 0.5);

    /* the old entry point stays a 1024 compatibility wrapper */
    d->dyn_rng_sgn[0] = 0;
    fill_spec(spec, 1024, 1.0);
    drc_decode(d, spec);
    TASK_CHECK(spec_to_double(spec[0]) == 2.0);
    TASK_CHECK(spec_to_double(spec[1023]) == 2.0);

    /* 960 frame: lines past the frame end stay untouched */
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 960) == 0);
    for (i = 0; i < 960; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);
    for (i = 960; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    drc_end(d);
    return 0;
}

static int gain_fractional_control(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];
    static const struct { double ctrl; int sgn; int ctl; } cases[] = {
        { 0.5,  0, 24 },  /* boost 2^0.5  */
        { 0.25, 0, 24 },  /* boost 2^0.25 */
        { 0.5,  1, 24 },  /* compress 2^-0.5 */
        { 0.75, 0, 36 },  /* boost 2^1.125 */
        { 1.0,  0, 12 },  /* boost 2^0.5 via ctl */
        { 0.3333333333333333, 1, 48 } /* fractional compress */
    };
    static const double vals[] = { 1.0, -1.0, 0.5, -3.25, 7.0, -0.125 };
    unsigned c, v;

    TASK_CHECK(d != NULL);
    d->num_bands = 1;

    for (c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
    {
        double g;
        d->ctrl1 = REAL_CONST(cases[c].ctrl);
        d->ctrl2 = REAL_CONST(cases[c].ctrl);
        d->dyn_rng_sgn[0] = (uint8_t)cases[c].sgn;
        d->dyn_rng_ctl[0] = (uint8_t)cases[c].ctl;
        /* recompute the expectation from the same (possibly rounded)
           control value the library sees */
        g = ref_gain(spec_to_double(d->ctrl1), cases[c].sgn,
                     cases[c].ctl, DRC_REF_LEVEL);
        for (v = 0; v < sizeof(vals)/sizeof(vals[0]); v++)
        {
            fill_spec(spec, 1024, vals[v]);
            TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
            TASK_CHECK(gain_matches(spec_to_double(spec[0]), vals[v] * g));
            TASK_CHECK(gain_matches(spec_to_double(spec[1023]), vals[v] * g));
        }
    }

    drc_end(d);
    return 0;
}

static int gain_reference_level(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];

    TASK_CHECK(d != NULL);
    d->num_bands = 1;

    /* reference loudness correction must not vanish with the control:
       ctrl 0, ref 76 -> gain 2^(-4/24) on both signs */
    d->ctrl1 = REAL_CONST(0.0);
    d->ctrl2 = REAL_CONST(0.0);
    d->prog_ref_level = 76;
    d->dyn_rng_ctl[0] = 24;

    d->dyn_rng_sgn[0] = 0;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            ref_gain(0.0, 0, 24, 76)));

    d->dyn_rng_sgn[0] = 1;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            ref_gain(0.0, 1, 24, 76)));

    /* fractional control plus reference correction: (0.5*24 - 4)/24 */
    d->ctrl2 = REAL_CONST(0.5);
    d->dyn_rng_sgn[0] = 0;
    fill_spec(spec, 1024, -2.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            -2.0 * ref_gain(0.5, 0, 24, 76)));

    /* reference level above 80 flips the correction sign */
    d->prog_ref_level = 88;
    d->ctrl2 = REAL_CONST(0.5);
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            ref_gain(0.5, 0, 24, 88)));

    /* ref 80 with ctl 0 is exactly neutral */
    d->prog_ref_level = DRC_REF_LEVEL;
    d->ctrl2 = REAL_CONST(1.0);
    d->dyn_rng_ctl[0] = 0;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(spec_to_double(spec[0]) == 1.0);

    drc_end(d);
    return 0;
}

static int gain_bands(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];
    int i;

    TASK_CHECK(d != NULL);

    /* two bands: [0,240) boost x2, [240,end) compress x0.5 */
    d->num_bands = 2;
    d->band_top[0] = 59;   /* 4*(59+1) = 240 */
    d->band_top[1] = 255;
    d->dyn_rng_sgn[0] = 0; d->dyn_rng_ctl[0] = 24;
    d->dyn_rng_sgn[1] = 1; d->dyn_rng_ctl[1] = 24;

    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    for (i = 0; i < 240; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);
    for (i = 240; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 0.5);

    /* same bands on a 960 frame: the last endpoint clamps to 960 and
       nothing past the frame end is touched */
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 960) == 0);
    for (i = 0; i < 240; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);
    for (i = 240; i < 960; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 0.5);
    for (i = 960; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    /* lines beyond the last band endpoint are left alone */
    d->band_top[1] = 119; /* 4*(119+1) = 480 */
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    for (i = 0; i < 240; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);
    for (i = 240; i < 480; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 0.5);
    for (i = 480; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    /* a single band covers the whole frame, its band_top is ignored */
    d->num_bands = 1;
    d->band_top[0] = 3;
    d->dyn_rng_sgn[0] = 0;
    d->dyn_rng_ctl[0] = 24;
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    for (i = 0; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);

    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 960) == 0);
    for (i = 0; i < 960; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 2.0);
    for (i = 960; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    drc_end(d);
    return 0;
}

static int gain_invalid(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];
    int i;

    TASK_CHECK(d != NULL);
    d->num_bands = 1;
    d->dyn_rng_sgn[0] = 0;
    d->dyn_rng_ctl[0] = 24;

    /* bad arguments: non-zero return, spectrum untouched */
    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(NULL, spec, 1024) != 0);
    TASK_CHECK(drc_decode_frame(d, NULL, 1024) != 0);
    TASK_CHECK(drc_decode_frame(d, spec, 0) != 0);
    TASK_CHECK(drc_decode_frame(d, spec, 1000) != 0);
    TASK_CHECK(drc_decode_frame(d, spec, 512) != 0);
    for (i = 0; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    /* band count out of range */
    d->num_bands = 0;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) != 0);
    d->num_bands = 18;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) != 0);
    for (i = 0; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    /* raw band endpoints must be strictly increasing; clamping must
       not be abused to hide a reversed or equal order */
    d->num_bands = 2;
    d->band_top[0] = 100;
    d->band_top[1] = 100;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) != 0);
    d->band_top[0] = 200;
    d->band_top[1] = 100;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) != 0);
    d->band_top[0] = 255;
    d->band_top[1] = 255; /* would clamp to identical endpoints */
    TASK_CHECK(drc_decode_frame(d, spec, 1024) != 0);
    for (i = 0; i < 1024; i++)
        TASK_CHECK(spec_to_double(spec[i]) == 1.0);

    /* a valid call still works afterwards */
    d->num_bands = 1;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(spec_to_double(spec[0]) == 2.0);

    drc_end(d);
    return 0;
}

static int gain_saturation(void)
{
    drc_info *d = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));
    real_t spec[1024];

    TASK_CHECK(d != NULL);
    d->num_bands = 1;
    d->dyn_rng_sgn[0] = 0;
    d->dyn_rng_ctl[0] = 127; /* gain 2^(127/24) ~= 39.3 */

    fill_spec(spec, 1024, 1.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    /* large but representable: check against the double formula */
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            ref_gain(1.0, 0, 127, DRC_REF_LEVEL)));

#ifdef FIXED_POINT
    /* Q14 spectrum times a large gain must saturate, not wrap */
    fill_spec(spec, 1024, 1.0);
    spec[0] = (real_t)0x10000000;
    spec[1] = (real_t)(int32_t)0xF0000000;
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(spec[0] == (real_t)0x7FFFFFFF);
    TASK_CHECK(spec[1] == (real_t)(int32_t)0x80000000);
    /* the remaining lines took the gain without saturating */
    for (int i = 2; i < 1024; i++)
        TASK_CHECK(gain_matches(spec_to_double(spec[i]),
                                ref_gain(1.0, 0, 127, DRC_REF_LEVEL)));
#else
    /* floating point does not saturate */
    fill_spec(spec, 1024, 1.0);
    spec[0] = REAL_CONST(16384.0);
    spec[1] = REAL_CONST(-16384.0);
    TASK_CHECK(drc_decode_frame(d, spec, 1024) == 0);
    TASK_CHECK(gain_matches(spec_to_double(spec[0]),
                            16384.0 * ref_gain(1.0, 0, 127, DRC_REF_LEVEL)));
    TASK_CHECK(gain_matches(spec_to_double(spec[1]),
                            -16384.0 * ref_gain(1.0, 0, 127, DRC_REF_LEVEL)));
#endif

    drc_end(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* metadata group                                                      */
/* ------------------------------------------------------------------ */

static int metadata_init_defaults(void)
{
    drc_info *d = drc_init(REAL_CONST(0.5), REAL_CONST(0.25));
    int i;

    TASK_CHECK(d != NULL);
    /* same defaults a fresh payload falls back to */
    TASK_CHECK(d->prog_ref_level == DRC_REF_LEVEL);
    TASK_CHECK(d->num_bands == 1);
    TASK_CHECK(d->excluded_chns_present == 0);
    for (i = 0; i < MAX_CHANNELS; i++)
        TASK_CHECK(d->exclude_mask[i] == 0);
    /* instance control values are kept as given */
    TASK_CHECK(d->ctrl1 == REAL_CONST(0.5));
    TASK_CHECK(d->ctrl2 == REAL_CONST(0.25));

    drc_end(d);
    return 0;
}

static int metadata_payload_reset(void)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long sr;
    unsigned char ch;
    drc_info *d;
    bitfile ld;
    uint8_t buf[256];
    uint32_t len;
    drc_desc full, minimal, other;
    int i;

    TASK_CHECK(open_decoder(ASC_MONO_1024, &h, &sr, &ch));

    d = drc_init(REAL_CONST(0.5), REAL_CONST(0.5));
    TASK_CHECK(d != NULL);

    /* full payload: 2 bands, prog_ref 76, excluded channel 0 */
    memset(&full, 0, sizeof(full));
    full.has_tag = 1;
    full.tag = 3;
    full.has_excl = 1;
    full.excl_mask[0] = 1;
    full.has_bands = 1;
    full.band_incr = 1; /* 2 bands */
    full.band_top[0] = 59;
    full.band_top[1] = 255;
    full.has_ref = 1;
    full.ref = 76;
    full.sgn[0] = 0; full.ctl[0] = 24;
    full.sgn[1] = 1; full.ctl[1] = 12;

    len = gen_fill_block(buf, &full, NULL);
    faad_initbits(&ld, buf, len);
    raw_data_block((NeAACDecStruct*)h, &info, &ld, NULL, d);
    TASK_CHECK(info.error == 0);
    faad_endbits(&ld);

    TASK_CHECK(d->present == 1);
    TASK_CHECK(d->num_bands == 2);
    TASK_CHECK(d->band_top[0] == 59 && d->band_top[1] == 255);
    TASK_CHECK(d->prog_ref_level == 76);
    TASK_CHECK(d->excluded_chns_present == 1);
    TASK_CHECK(d->exclude_mask[0] == 1);
    TASK_CHECK(d->dyn_rng_sgn[0] == 0 && d->dyn_rng_ctl[0] == 24);
    TASK_CHECK(d->dyn_rng_sgn[1] == 1 && d->dyn_rng_ctl[1] == 12);

    /* minimal payload: everything omitted -> defaults, no inheritance */
    memset(&minimal, 0, sizeof(minimal));
    minimal.sgn[0] = 1;
    minimal.ctl[0] = 40;

    len = gen_fill_block(buf, &minimal, NULL);
    faad_initbits(&ld, buf, len);
    raw_data_block((NeAACDecStruct*)h, &info, &ld, NULL, d);
    TASK_CHECK(info.error == 0);
    faad_endbits(&ld);

    TASK_CHECK(d->present == 1);
    TASK_CHECK(d->num_bands == 1);
    TASK_CHECK(d->prog_ref_level == DRC_REF_LEVEL);
    TASK_CHECK(d->excluded_chns_present == 0);
    for (i = 0; i < MAX_CHANNELS; i++)
        TASK_CHECK(d->exclude_mask[i] == 0);
    TASK_CHECK(d->dyn_rng_sgn[0] == 1 && d->dyn_rng_ctl[0] == 40);
    /* instance control values survive any payload */
    TASK_CHECK(d->ctrl1 == REAL_CONST(0.5));
    TASK_CHECK(d->ctrl2 == REAL_CONST(0.5));

    /* two consecutive payloads inside a single fill element: the
       second one is a complete new description */
    memset(&other, 0, sizeof(other));
    other.has_bands = 1;
    other.band_incr = 2; /* 3 bands */
    other.band_top[0] = 9;
    other.band_top[1] = 59;
    other.band_top[2] = 255;
    other.has_ref = 1;
    other.ref = 88;
    other.has_excl = 1;
    other.excl_mask[1] = 1;
    other.sgn[0] = 0; other.ctl[0] = 8;
    other.sgn[1] = 0; other.ctl[1] = 16;
    other.sgn[2] = 1; other.ctl[2] = 32;

    len = gen_fill_block(buf, &other, &minimal);
    faad_initbits(&ld, buf, len);
    raw_data_block((NeAACDecStruct*)h, &info, &ld, NULL, d);
    TASK_CHECK(info.error == 0);
    faad_endbits(&ld);

    /* only the second (minimal) description may be in effect */
    TASK_CHECK(d->num_bands == 1);
    TASK_CHECK(d->prog_ref_level == DRC_REF_LEVEL);
    TASK_CHECK(d->excluded_chns_present == 0);
    TASK_CHECK(d->exclude_mask[1] == 0);
    TASK_CHECK(d->dyn_rng_sgn[0] == 1 && d->dyn_rng_ctl[0] == 40);

    /* a block without a new extension keeps the active info */
    len = gen_fill_block(buf, NULL, NULL);
    faad_initbits(&ld, buf, len);
    raw_data_block((NeAACDecStruct*)h, &info, &ld, NULL, d);
    TASK_CHECK(info.error == 0);
    faad_endbits(&ld);

    TASK_CHECK(d->present == 1);
    TASK_CHECK(d->num_bands == 1);
    TASK_CHECK(d->dyn_rng_ctl[0] == 40);

    drc_end(d);
    NeAACDecClose(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* stream group                                                        */
/* ------------------------------------------------------------------ */

/* nonzero spectral line positions produced by Q_POS / Q_NEG */
static const int POS_A[4] = { 0, 5, 10, 15 };
static const int POS_B[4] = { 3, 6, 9, 12 };

static int stream_mono_1024(void)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long sr;
    unsigned char ch;
    uint8_t buf[1024];
    uint32_t len;
    drc_desc dd;
    int i;

    obs_reset();
    TASK_CHECK(open_decoder(ASC_MONO_1024, &h, &sr, &ch));
    TASK_CHECK(sr == 44100 && ch == 1);

    /* primer frame: the very first decoded frame yields no samples */
    len = gen_frame(buf, 0, Q_POS, NULL, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    obs_reset();

    /* control frame without DRC */
    len = gen_frame(buf, 0, Q_POS, NULL, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);

    /* frame with a DRC boost of 2 */
    memset(&dd, 0, sizeof(dd));
    dd.sgn[0] = 0;
    dd.ctl[0] = 24;
    len = gen_frame(buf, 0, Q_POS, NULL, 4, &dd);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);

    /* frame without new metadata: the active info stays in effect */
    len = gen_frame(buf, 0, Q_POS, NULL, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);

    TASK_CHECK(g_obs_count == 3);
    TASK_CHECK(g_obs_len[0] == 1024 && g_obs_len[1] == 1024 && g_obs_len[2] == 1024);

    for (i = 0; i < 4; i++)
    {
        int p = POS_A[i];
        TASK_CHECK(fabs(g_obs[0][p]) > 0.01); /* nonzero spectrum */
        TASK_CHECK(fabs(g_obs[1][p] / g_obs[0][p] - 2.0) < 1e-6);
        TASK_CHECK(fabs(g_obs[2][p] / g_obs[0][p] - 2.0) < 1e-6);
    }
    for (i = 16; i < 1024; i++)
        TASK_CHECK(g_obs[1][i] == 0.0);

    NeAACDecClose(h);
    return 0;
}

static int stream_mono_960(void)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long sr;
    unsigned char ch;
    uint8_t buf[1024];
    uint32_t len;
    drc_desc dd;
    int i;

    obs_reset();
    TASK_CHECK(open_decoder(ASC_MONO_960, &h, &sr, &ch));
    TASK_CHECK(sr == 44100 && ch == 1);

    /* primer frame: the very first decoded frame yields no samples */
    len = gen_frame(buf, 0, Q_POS, NULL, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    obs_reset();

    /* control frame without DRC */
    len = gen_frame(buf, 0, Q_POS, NULL, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 960);

    /* DRC boost of 4 (ctl 48) */
    memset(&dd, 0, sizeof(dd));
    dd.sgn[0] = 0;
    dd.ctl[0] = 48;
    len = gen_frame(buf, 0, Q_POS, NULL, 4, &dd);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 960);

    TASK_CHECK(g_obs_count == 2);
    TASK_CHECK(g_obs_len[0] == 960 && g_obs_len[1] == 960);

    for (i = 0; i < 4; i++)
    {
        int p = POS_A[i];
        TASK_CHECK(fabs(g_obs[0][p]) > 0.01);
        TASK_CHECK(fabs(g_obs[1][p] / g_obs[0][p] - 4.0) < 1e-6);
    }
    for (i = 16; i < 960; i++)
        TASK_CHECK(g_obs[1][i] == 0.0);

    NeAACDecClose(h);
    return 0;
}

static int stream_stereo_exclude(void)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long sr;
    unsigned char ch;
    uint8_t buf[1024];
    uint32_t len;
    drc_desc dd;
    int i;

    obs_reset();
    TASK_CHECK(open_decoder(ASC_STEREO_1024, &h, &sr, &ch));
    TASK_CHECK(sr == 44100 && ch == 2);

    /* primer frame: the very first decoded frame yields no samples */
    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    obs_reset();

    /* control frame, left and right carry different spectra */
    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 2048);

    /* DRC boost of 2 with channel 1 excluded */
    memset(&dd, 0, sizeof(dd));
    dd.has_excl = 1;
    dd.excl_mask[1] = 1;
    dd.sgn[0] = 0;
    dd.ctl[0] = 24;
    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, &dd);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);

    TASK_CHECK(g_obs_count == 4);
    TASK_CHECK(g_obs_ch[0] == 0 && g_obs_ch[1] == 1);
    TASK_CHECK(g_obs_ch[2] == 0 && g_obs_ch[3] == 1);

    for (i = 0; i < 4; i++)
    {
        int pa = POS_A[i], pb = POS_B[i];
        TASK_CHECK(fabs(g_obs[0][pa]) > 0.01);
        TASK_CHECK(fabs(g_obs[1][pb]) > 0.01);
        TASK_CHECK(g_obs[1][pb] < 0.0); /* right channel is negative */
        TASK_CHECK(g_obs[0][pa] != g_obs[1][pa]); /* L != R */
        /* boosted left channel */
        TASK_CHECK(fabs(g_obs[2][pa] / g_obs[0][pa] - 2.0) < 1e-6);
        /* excluded right channel is passed through untouched */
        TASK_CHECK(g_obs[3][pb] == g_obs[1][pb]);
    }

    NeAACDecClose(h);
    return 0;
}

static int stream_stereo_960_no_drc(void)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long sr;
    unsigned char ch;
    uint8_t buf[1024];
    uint32_t len;
    int i;

    obs_reset();
    TASK_CHECK(open_decoder(ASC_STEREO_960, &h, &sr, &ch));
    TASK_CHECK(sr == 44100 && ch == 2);

    /* primer frame: the very first decoded frame yields no samples */
    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    obs_reset();

    /* two identical frames, never any DRC metadata */
    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1920);

    len = gen_frame(buf, 1, Q_POS, Q_NEG, 4, NULL);
    NeAACDecDecode(h, &info, buf, len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1920);

    TASK_CHECK(g_obs_count == 4);
    TASK_CHECK(g_obs_len[0] == 960 && g_obs_len[2] == 960);

    for (i = 0; i < 4; i++)
    {
        int pa = POS_A[i], pb = POS_B[i];
        TASK_CHECK(fabs(g_obs[0][pa]) > 0.01);
        TASK_CHECK(fabs(g_obs[1][pb]) > 0.01);
        /* no DRC anywhere: identical frames decode identically */
        TASK_CHECK(g_obs[2][pa] == g_obs[0][pa]);
        TASK_CHECK(g_obs[3][pb] == g_obs[1][pb]);
    }

    NeAACDecClose(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* case table                                                          */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "gain", "boost_compress", gain_boost_compress },
    { "gain", "fractional_control", gain_fractional_control },
    { "gain", "reference_level", gain_reference_level },
    { "gain", "bands", gain_bands },
    { "gain", "invalid", gain_invalid },
    { "gain", "saturation", gain_saturation },
    { "metadata", "init_defaults", metadata_init_defaults },
    { "metadata", "payload_reset", metadata_payload_reset },
    { "stream", "mono_1024", stream_mono_1024 },
    { "stream", "mono_960", stream_mono_960 },
    { "stream", "stereo_exclude", stream_stereo_exclude },
    { "stream", "stereo_960_no_drc", stream_stereo_960_no_drc }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
