/*
    tests/layer2_contract.c

    Independent fixed-point contract for the MPEG Audio Layer II branch of
    minimp3: grouped (3/5/9 level) quantization, SCFSI scale-factor reuse and
    full frame decode.  The production decoder, its public API and the existing
    PCM/PSNR tests are untouched; this file only adds read-only regression
    checks that localize a difference to a codeword, subband or consumed bit
    count instead of re-decoding PCM.

    The independent model below uses its own MSB-first bit writer/reader and its
    own arithmetic for unpacking, de-centering and scale factors.  It never calls
    the production get_bits(), group splitter or dequantizer to produce expected
    values and never caches production output as the reference.  The only
    production entry points exercised are the real L12_* helpers and
    mp3dec_decode_frame(), which are the code under test.  Allocation/table
    constants are duplicated here as format data.
*/

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_L2_TEST_OBSERVE
void mp3_l2_observe(int igr, const void *sci, const float *grbuf);
#include "minimp3.h"

#include "check.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Independent bit plumbing (MSB first, never used by production).    */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *buf;
    int pos, cap_bits;
} l2_bw_t;

static void l2_bw_init(l2_bw_t *w, unsigned char *buf, int cap_bytes)
{
    w->buf = buf; w->pos = 0; w->cap_bits = cap_bytes*8;
}

static void l2_bw_put(l2_bw_t *w, unsigned value, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--)
    {
        if (w->pos >= w->cap_bits)
        {
            TASK_CHECK(0, "writer overflow");
            return;
        }
        if ((value >> i) & 1u)
            w->buf[w->pos >> 3] |= (unsigned char)(1u << (7 - (w->pos & 7)));
        w->pos++;
    }
}

typedef struct
{
    const unsigned char *buf;
    int pos;
} l2_rd_t;

static unsigned l2_rd_get(l2_rd_t *r, int n)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        v = (v << 1) | (unsigned)((r->buf[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
        r->pos++;
    }
    return v;
}

static void l2_msg(char *out, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Layer II format tables (format data, duplicated from ISO 11172-3). */
/* ------------------------------------------------------------------ */

static const uint8_t l2_bitalloc_code_tab[] = {
    0,17, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16,
    0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,16,
    0,17,18, 3,19,4,5,16,
    0,17,18,16,
    0,17,18,19, 4,5,6, 7,8, 9,10,11,12,13,14,15,
    0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,14,
    0, 2, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16
};

static const L12_subband_alloc_t l2_alloc_mpeg1[] = {
    { 0, 4, 3 }, { 16, 4, 8 }, { 32, 3, 12 }, { 40, 2, 7 }
};

#define L2_NB           30   /* subbands for the chosen 192 kbps MPEG-1 frames */
#define L2_FRAME_BYTES  626  /* 192 kbps, 44.1 kHz, Layer II, no padding       */
#define L2_NGR          3
#define L2_NCH          2

/* modes used by the synthetic frames */
enum { L2_MODE_MONO = 0, L2_MODE_STEREO = 2, L2_MODE_JOINT = 1 };

typedef struct
{
    int mode, joint_ext, sb_stereo;
    int baL[L2_NB], baR[L2_NB];
    int scfL[L2_NB], scfR[L2_NB];
    int indices[5];
    int seed;
} l2_plan_t;

static int l2_ba_table(int sb, const uint8_t **tab, int *width)
{
    int k = 0, i;
    const L12_subband_alloc_t *a = l2_alloc_mpeg1;
    *tab = l2_bitalloc_code_tab; *width = 0;
    for (i = 0; i < L2_NB; i++)
    {
        if (i == k)
        {
            *width = a->code_tab_width;
            *tab = l2_bitalloc_code_tab + a->tab_offset;
            k += a->band_count;
            a++;
        }
        if (i == sb)
            return 0;
    }
    return -1;
}

static int l2_ba_width(int sb)
{
    const uint8_t *tab;
    int width;
    l2_ba_table(sb, &tab, &width);
    return width;
}

static int l2_find_ba_code(int sb, int ba)
{
    const uint8_t *tab;
    int width, c;
    l2_ba_table(sb, &tab, &width);
    for (c = 0; c < (1 << width); c++)
        if (tab[c] == ba)
            return c;
    return -1;
}

/* quantization levels of a bit-allocation value */
static int l2_levels(int ba)
{
    return ba < 17 ? (1 << ba) - 1 : (2 << (ba - 17)) + 1;   /* non-grouped: 3/7/15..., grouped: 3/5/9 */
}

static int l2_scf_fields(int scfcod)
{
    static const int cnt[4] = { 3, 2, 1, 2 };
    return cnt[scfcod];
}

/* which transmitted field (0..2) feeds each of the three granule sections */
static int l2_scf_section(int scfcod, int section)
{
    static const int map[4][3] = { {0,1,2}, {0,0,1}, {0,0,0}, {0,1,1} };
    return map[scfcod][section];
}

/*
 * Deterministic scale-factor index for transmitted field q of slot
 * (2*sb + ch).  Left and right draw from disjoint index sets so a shared
 * joint-stereo codeword always keeps two genuinely different scale sets; the
 * five required indices are covered across both channels.
 */
static int l2_field_index(const l2_plan_t *p, int slot, int q)
{
    static const int left_pick[3]  = { 0, 2, 3 };   /* indices 0, 2, 62 */
    static const int right_pick[3] = { 1, 4, 1 };   /* indices 1, 63, 1 */
    return p->indices[(slot & 1) ? right_pick[q % 3] : left_pick[q % 3]];
}

/*
 * Effective right-channel allocation in the production parse order.  In joint
 * stereo the right slots past stereo_bands keep a stale copy of the left
 * allocation while SCFSI/scale factors are read (the zeroing happens later), so
 * a shared codeword still carries a separate right scale set.
 */
static int l2_eff_baR(const l2_plan_t *p, int sb)
{
    if (p->mode == L2_MODE_MONO)
        return 0;
    if (sb < p->sb_stereo)
        return p->baR[sb];
    return p->mode == L2_MODE_JOINT ? p->baL[sb] : 0;
}

static void l2_make_header(const l2_plan_t *p, unsigned char hdr[4])
{
    hdr[0] = 0xff; hdr[1] = 0xfd; hdr[2] = 0xa0;   /* MPEG-1, Layer II, 192k, 44.1k */
    hdr[3] = p->mode == L2_MODE_MONO ? 0xc0 :
             p->mode == L2_MODE_JOINT ? (unsigned char)(0x40 | (p->joint_ext << 4)) : 0x00;
}

/* Build one frame payload (everything after the 4-byte header) from a plan. */
static void l2_build_payload(const l2_plan_t *p, unsigned char *payload)
{
    l2_bw_t w;
    int sb, i, g, j, q;
    l2_bw_init(&w, payload, L2_FRAME_BYTES - 4);

    /* bit allocation */
    for (sb = 0; sb < L2_NB; sb++)
    {
        l2_bw_put(&w, (unsigned)l2_find_ba_code(sb, p->baL[sb]), l2_ba_width(sb));
        if (sb < p->sb_stereo)
            l2_bw_put(&w, (unsigned)l2_find_ba_code(sb, p->baR[sb]), l2_ba_width(sb));
    }

    /* SCFSI bits (stale right slots in joint stereo included) */
    for (i = 0; i < 2*L2_NB; i++)
    {
        sb = i >> 1;
        int ba = (i & 1) ? l2_eff_baR(p, sb) : p->baL[sb];
        if (ba)
            l2_bw_put(&w, (unsigned)((i & 1) ? p->scfR[sb] : p->scfL[sb]), 2);
    }

    /* scale factor indices, deliberately distinct per section */
    for (i = 0; i < 2*L2_NB; i++)
    {
        sb = i >> 1;
        int ba = (i & 1) ? l2_eff_baR(p, sb) : p->baL[sb];
        if (!ba)
            continue;
        int sc = (i & 1) ? p->scfR[sb] : p->scfL[sb];
        for (q = 0; q < l2_scf_fields(sc); q++)
            l2_bw_put(&w, (unsigned)l2_field_index(p, i, q), 6);
    }

    /* samples: 3 granules, 4 groups of 3, channel-interleaved per subband.
       Beyond the joint boundary the right codeword is absent (the shared left
       codeword is read only once); the stale allocation only feeds scale sets. */
    for (g = 0; g < L2_NGR; g++)
        for (j = 0; j < 4; j++)
            for (i = 0; i < 2*L2_NB; i++)
            {
                sb = i >> 1;
                int ch = i & 1;
                int ba = ch ? p->baR[sb] : p->baL[sb];
                if (!ba)
                    continue;
                if (ba < 17)
                {
                    for (q = 0; q < 3; q++)
                        l2_bw_put(&w, (unsigned)((g*5 + j*3 + q + p->seed) & ((1 << ba) - 1)), ba);
                } else
                {
                    int mod = (2 << (ba - 17)) + 1;
                    int width = mod + 2 - (mod >> 3);
                    unsigned span = (unsigned)mod*mod*mod;
                    unsigned code = 1u + (unsigned)(g*7 + j*13 + i + p->seed) % (span - 1);
                    l2_bw_put(&w, code, width);
                }
            }
}

/* Independent model results for one planned frame. */
typedef struct
{
    int      baL[L2_NB], baR[L2_NB];
    int      scfL[L2_NB], scfR[L2_NB];
    double   scf[L2_NCH][L2_NB][3];
    float    raw[L2_NGR][L2_NCH][576];      /* de-centered, pre-scale-factor */
    int      field_count;                   /* 6-bit scale factor fields read */
    int      pos_after_scf;                 /* bits consumed through scale factors */
    int      pos_after_gr[L2_NGR];          /* bits consumed after each granule */
    int      grouped_words;
    unsigned last_codeword;                /* exact integer of last grouped word */
    int      last_codeword_pos[2];          /* its [start,end) bit positions */
} l2_model_t;

static void l2_model_run(const l2_plan_t *p, const unsigned char *payload, l2_model_t *m)
{
    l2_rd_t r = { payload, 0 };
    int sb, i, g, j, q, ch;

    memset(m, 0, sizeof(*m));

    /* bit allocation read-back */
    for (sb = 0; sb < L2_NB; sb++)
    {
        const uint8_t *tab; int width;
        l2_ba_table(sb, &tab, &width);
        m->baL[sb] = tab[l2_rd_get(&r, width)];
        m->baR[sb] = sb < p->sb_stereo ? tab[l2_rd_get(&r, width)] : 0;
    }

    /* SCFSI read-back */
    for (i = 0; i < 2*L2_NB; i++)
    {
        sb = i >> 1;
        int ba = (i & 1) ? l2_eff_baR(p, sb) : p->baL[sb];
        int v = ba ? (int)l2_rd_get(&r, 2) : 6;
        if (i & 1) m->scfR[sb] = v; else m->scfL[sb] = v;
    }

    /* scale factors: double precision reference 2*2^(-index/3)/levels */
    for (i = 0; i < 2*L2_NB; i++)
    {
        sb = i >> 1; ch = i & 1;
        int ba = ch ? l2_eff_baR(p, sb) : p->baL[sb];
        if (!ba)
            continue;
        int sc = ch ? p->scfR[sb] : p->scfL[sb];
        int nf = l2_scf_fields(sc);
        double levels = (double)l2_levels(ba), fv[3] = { 0, 0, 0 };
        for (q = 0; q < nf; q++)
        {
            int idx = (int)l2_rd_get(&r, 6);
            TASK_CHECK(idx == l2_field_index(p, i, q), "wire scale index matches plan");
            m->field_count++;
            fv[q] = 2.0*pow(2.0, -idx/3.0)/levels;
        }
        for (q = 0; q < 3; q++)
            m->scf[ch][sb][q] = fv[l2_scf_section(sc, q)];
    }
    m->pos_after_scf = r.pos;

    /* samples (the joint-shared right slots read no codeword) */
    for (g = 0; g < L2_NGR; g++)
    {
        for (j = 0; j < 4; j++)
            for (i = 0; i < 2*L2_NB; i++)
            {
                sb = i >> 1; ch = i & 1;
                int ba = ch ? p->baR[sb] : p->baL[sb];
                if (!ba)
                    continue;
                if (ba < 17)
                {
                    int half = (1 << (ba - 1)) - 1;
                    for (q = 0; q < 3; q++)
                    {
                        int code = (int)l2_rd_get(&r, ba);
                        m->raw[g][ch][sb*18 + j*3 + q] = (float)(code - half);
                    }
                } else
                {
                    int mod = (2 << (ba - 17)) + 1;
                    int width = mod + 2 - (mod >> 3);
                    int start = r.pos;
                    unsigned code = l2_rd_get(&r, width);
                    unsigned repack = 0, place = 1;
                    int digit;
                    m->grouped_words++;
                    m->last_codeword = code;
                    m->last_codeword_pos[0] = start;
                    m->last_codeword_pos[1] = r.pos;
                    for (q = 0; q < 3; q++)
                    {
                        digit = (int)(code % mod);
                        m->raw[g][ch][sb*18 + j*3 + q] = (float)(digit - mod/2);
                        repack += (unsigned)digit*place;
                        place *= (unsigned)mod;
                        code /= (unsigned)mod;
                    }
                    /* exact integer identity: digits must re-pack to the word */
                    TASK_CHECK(repack == m->last_codeword, "grouped word re-pack mismatch");
                }
            }
        m->pos_after_gr[g] = r.pos;
    }
}

/* Model post-scale-factor value at grbuf[ch*576 + sb*18 + t]. */
static double l2_model_value(const l2_plan_t *p, const l2_model_t *m, int g, int ch, int sb, int t)
{
    int src_ch = (ch == 1 && p->mode == L2_MODE_JOINT && sb >= p->sb_stereo) ? 0 : ch;
    return (double)m->raw[g][src_ch][sb*18 + t] * m->scf[ch][sb][g];
}

/* ------------------------------------------------------------------ */
/* Test-only observation hook (read-only, compiled out normally).      */
/* ------------------------------------------------------------------ */

#define L2_OBS_GRANULES 3
static int   l2_obs_count;
static float l2_obs_grbuf[L2_OBS_GRANULES][1152];
static int   l2_obs_total, l2_obs_stereo;

void mp3_l2_observe(int igr, const void *vsci, const float *grbuf)
{
    const L12_scale_info *sci = (const L12_scale_info *)vsci;
    if (igr < 0 || igr >= L2_OBS_GRANULES)
        return;
    l2_obs_total = sci->total_bands;
    l2_obs_stereo = sci->stereo_bands;
    memcpy(l2_obs_grbuf[igr], grbuf, sizeof(l2_obs_grbuf[0]));
    l2_obs_count++;
}

static int l2_float_close(double got, double want)
{
    return fabs(got - want) <= 2e-6*(1.0 + fabs(want));
}

/* Compare every post-scale-factor subband value of one granule. */
static void l2_compare_granule(const l2_plan_t *p, const l2_model_t *m, int g, const float *gr)
{
    int nch = p->mode == L2_MODE_MONO ? 1 : 2;
    int ch, sb, t;
    char msg[192];
    for (ch = 0; ch < nch; ch++)
        for (sb = 0; sb < L2_NB; sb++)
            for (t = 0; t < 18; t++)
            {
                double want = l2_model_value(p, m, g, ch, sb, t);
                float got = gr[ch*576 + sb*18 + t];
                if (!l2_float_close((double)got, want))
                {
                    l2_msg(msg, sizeof(msg),
                           "post-scf mismatch g=%d ch=%d sb=%d t=%d", g, ch, sb, t);
                    TASK_CHECK(0, msg);
                }
            }
}

/* ------------------------------------------------------------------ */
/* digits group: exhaustive 3/5/9-level grouped codeword contract.     */
/* ------------------------------------------------------------------ */

/*
 * One synthetic dequantize pass uses three subbands:
 *   sb 0: ordinary non-grouped ba=4 samples (leading)
 *   sb 1: the grouped ba under test (17/18/19)
 *   sb 2: ordinary non-grouped ba=3 samples (trailing)
 *   sb 3: ba=0 zero subband
 * Every legal ternary is enumerated; the grouped word is built bit-by-bit
 * from the explicit triple and the real L12_dequantize_granule() is then run.
 */
static int l2_group_digits(void)
{
    static const int grouped_ba[3] = { 17, 18, 19 };
    int case_idx, offset, s0, s1, s2, j, k;
    char msg[192];

    for (case_idx = 0; case_idx < 3; case_idx++)
    {
        int ba = grouped_ba[case_idx];
        int mod = (2 << (ba - 17)) + 1;              /* 3, 5, 9 */
        int width = mod + 2 - (mod >> 3);            /* 5, 7, 10 */
        for (offset = 0; offset < 8; offset++)
        {
            for (s0 = 0; s0 < mod; s0++)
                for (s1 = 0; s1 < mod; s1++)
                    for (s2 = 0; s2 < mod; s2++)
                    {
                        unsigned char payload[256];
                        unsigned char expect_bytes[2];
                        l2_bw_t w;
                        l2_rd_t rd;
                        L12_scale_info sci;
                        float gr[1152];
                        bs_t bs;
                        int expect_word = s0 + s1*mod + s2*mod*mod;
                        int lead_ba = 4, trail_ba = 3;
                        int lead_half = (1 << (lead_ba - 1)) - 1;
                        int trail_half = (1 << (trail_ba - 1)) - 1;
                        int total_bits, word_start, word_end;

                        memset(payload, 0, sizeof(payload));
                        memset(gr, 0, sizeof(gr));
                        l2_bw_init(&w, payload, sizeof(payload));
                        l2_bw_put(&w, 0, offset);       /* misalign across bytes */

                        /* explicit bit-by-bit construction of the word bytes */
                        expect_bytes[0] = expect_bytes[1] = 0;
                        for (k = width - 1; k >= 0; k--)
                        {
                            int bit = (expect_word >> k) & 1;
                            if (bit)
                                expect_bytes[k >> 3] |= (unsigned char)(1u << (k & 7));
                        }
                        /* the generated bytes must be the expect_word packed LSB-by-bit */
                        {
                            int rebuild = 0;
                            for (k = 0; k < width; k++)
                                rebuild |= ((expect_bytes[k >> 3] >> (k & 7)) & 1) << k;
                            TASK_CHECK(rebuild == expect_word, "bit-generated bytes pack to the word");
                        }

                        /* bitstream in the production read order: 4 groups of 3 samples */
                        for (j = 0; j < 4; j++)
                        {
                            int word_pos[2];
                            for (k = 0; k < 3; k++)
                                l2_bw_put(&w, (unsigned)((j + k + 1) & ((1 << lead_ba) - 1)), lead_ba);
                            word_pos[0] = w.pos;
                            l2_bw_put(&w, (unsigned)expect_word, width);
                            word_pos[1] = w.pos;
                            for (k = 0; k < 3; k++)
                                l2_bw_put(&w, (unsigned)((j + k + 2) & ((1 << trail_ba) - 1)), trail_ba);
                            if (j == 0) { word_start = word_pos[0]; word_end = word_pos[1]; }
                        }
                        total_bits = w.pos;

                        /* independent read-back of the word at its bit position */
                        rd.buf = payload; rd.pos = word_start;
                        {
                            unsigned read_word = 0;
                            for (k = 0; k < width; k++)
                                read_word = (read_word << 1) |
                                    (unsigned)((payload[rd.pos >> 3] >> (7 - (rd.pos & 7))) & 1), rd.pos++;
                            l2_msg(msg, sizeof(msg), "word integer ba=%d triple=(%d,%d,%d) off=%d",
                                   ba, s0, s1, s2, offset);
                            TASK_CHECK(read_word == (unsigned)expect_word, msg);
                            TASK_CHECK(rd.pos == word_end, "grouped word end bit position");
                        }

                        /*
                         * The production bitstream always starts at bit 0, so
                         * re-pack the same fields from the offset into a
                         * bit-0-aligned view for L12_dequantize_granule.  The
                         * offset buffer above still proves the model reads the
                         * word correctly across a byte boundary.
                         */
                        {
                            unsigned char aligned[256];
                            l2_bw_t aw;
                            l2_rd_t src;
                            int bit;
                            memset(aligned, 0, sizeof(aligned));
                            l2_bw_init(&aw, aligned, sizeof(aligned));
                            src.buf = payload; src.pos = offset;
                            for (bit = offset; bit < total_bits; bit++)
                                l2_bw_put(&aw, l2_rd_get(&src, 1), 1);
                            memset(&sci, 0, sizeof(sci));
                            sci.total_bands = 4;
                            sci.stereo_bands = 0;
                            sci.bitalloc[0] = (uint8_t)lead_ba;
                            sci.bitalloc[2] = (uint8_t)ba;
                            sci.bitalloc[4] = (uint8_t)trail_ba;
                            sci.bitalloc[6] = 0;

                            bs_init(&bs, aligned, (int)sizeof(aligned));
                            L12_dequantize_granule(gr, &bs, &sci, 3);
                            l2_msg(msg, sizeof(msg), "consumed bits ba=%d triple=(%d,%d,%d) off=%d",
                                   ba, s0, s1, s2, offset);
                            TASK_CHECK(bs.pos == total_bits - offset, msg);
                        }

                        /* sample order and de-centered integer values of the triple */
                        for (j = 0; j < 4; j++)
                        {
                            float got[3];
                            got[0] = gr[1*18 + j*3 + 0];
                            got[1] = gr[1*18 + j*3 + 1];
                            got[2] = gr[1*18 + j*3 + 2];
                            l2_msg(msg, sizeof(msg), "digits order/value ba=%d j=%d s0=%d s1=%d",
                                   ba, j, s0, s1);
                            TASK_CHECK(got[0] == (float)(s0 - mod/2), msg);
                            TASK_CHECK(got[1] == (float)(s1 - mod/2), msg);
                            TASK_CHECK(got[2] == (float)(s2 - mod/2), msg);
                        }

                        /* leading/trailing ordinary quantized samples */
                        for (j = 0; j < 4; j++)
                        {
                            for (k = 0; k < 3; k++)
                            {
                                int lead_code = (j + k + 1) & ((1 << lead_ba) - 1);
                                int trail_code = (j + k + 2) & ((1 << trail_ba) - 1);
                                l2_msg(msg, sizeof(msg), "plain lead ba=%d j=%d k=%d d=%d",
                                       ba, j, k, lead_code);
                                TASK_CHECK(gr[0*18 + j*3 + k] == (float)(lead_code - lead_half), msg);
                                TASK_CHECK(gr[2*18 + j*3 + k] == (float)(trail_code - trail_half),
                                           "plain trailing de-centered value");
                            }
                        }

                        /* the ba=0 subband consumes no bits and stays zero */
                        for (k = 0; k < 18; k++)
                        {
                            l2_msg(msg, sizeof(msg), "zero subband ba=%d k=%d sx=%d sy=%d",
                                   ba, k, s0, s1);
                            TASK_CHECK(gr[3*18 + k] == 0.0f, msg);
                        }

                        /* the word occupies exactly width bits at the planned spot */
                        l2_msg(msg, sizeof(msg), "word span ba=%d width=%d start=%d end=%d",
                               ba, width, word_start, word_end);
                        TASK_CHECK(word_end - word_start == width, msg);
                        TASK_CHECK(word_start >= offset, "word starts after alignment padding");
                    }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* scfsi group: the four reuse modes against the real scale parser.   */
/* ------------------------------------------------------------------ */

static void l2_fill_plan(l2_plan_t *p, int mode)
{
    int sb;
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->joint_ext = 1;
    p->sb_stereo = mode == L2_MODE_MONO ? 0 :
                   mode == L2_MODE_JOINT ? (p->joint_ext << 2) + 4 : L2_NB;
    p->indices[0] = 0; p->indices[1] = 1; p->indices[2] = 2;
    p->indices[3] = 62; p->indices[4] = 63;
    p->seed = 1;

    /* grouped (17/18/19) and ordinary (3/4) allocations, encodable per table */
    p->baL[0] = 17; p->baL[1] = 4;  p->baL[2] = 3;  p->baL[3] = 18;
    p->baL[4] = 19; p->baL[5] = 3;  p->baL[11] = 19; p->baL[23] = 18;
    if (mode != L2_MODE_MONO)
    {
        p->baR[0] = 17; p->baR[1] = 3; p->baR[3] = 18; p->baR[4] = 4;
        p->baR[11] = 19; p->baR[23] = 17;
        if (mode == L2_MODE_JOINT)
        {
            /* inside the joint boundary: right has its own codewords */
            for (sb = 0; sb < L2_NB; sb++)
                if (sb >= p->sb_stereo)
                    p->baR[sb] = 0;
            p->baR[5] = 4;
        }
    }
    /* all four SCFSI modes appear on active slots; L and R indices never match */
    for (sb = 0; sb < L2_NB; sb++)
    {
        p->scfL[sb] = (p->baL[sb] || (mode == L2_MODE_JOINT && sb >= p->sb_stereo)) ? sb % 4 : 6;
        p->scfR[sb] = (l2_eff_baR(p, sb)) ? (sb + 2) % 4 : 6;
    }
}

static int l2_group_scfsi(void)
{
    static const int modes[3] = { L2_MODE_MONO, L2_MODE_STEREO, L2_MODE_JOINT };
    int mi, g, sb, ch, i;
    char msg[192];
    static int l2_global_idx_seen[64];

    for (mi = 0; mi < 3; mi++)
    {
        l2_plan_t plan, *p = &plan;
        l2_model_t model;
        static unsigned char frame[L2_FRAME_BYTES];
        unsigned char *payload = frame + 4;
        unsigned char hdr[4];
        L12_scale_info sci;
        bs_t bs;
        float gr[1152];
        int modes_seen[4] = { 0, 0, 0, 0 };

        l2_fill_plan(p, modes[mi]);
        memset(frame, 0, sizeof(frame));
        l2_make_header(p, hdr);
        l2_build_payload(p, payload);
        l2_model_run(p, payload, &model);

        /* coverage: every SCFSI mode is exercised on an active slot */
        for (sb = 0; sb < L2_NB; sb++)
        {
            if (p->baL[sb]) modes_seen[p->scfL[sb]] = 1;
            if (l2_eff_baR(p, sb)) modes_seen[p->scfR[sb]] = 1;
        }
        for (i = 0; i < 4; i++)
        {
            l2_msg(msg, sizeof(msg), "scfcod mode %d unused in mode cfg=%d x=%d y=%d",
                   i, modes[mi], 0, 0);
            TASK_CHECK(modes_seen[i], msg);
        }
        /* index coverage across every active slot of this configuration, plus
           the exact number of 6-bit fields the plan says must be read */
        int expected_fields = 0;
        for (i = 0; i < 2*L2_NB; i++)
        {
            sb = i >> 1; ch = i & 1;
            int ba = ch ? l2_eff_baR(p, sb) : p->baL[sb];
            int sc = ch ? p->scfR[sb] : p->scfL[sb];
            if (!ba)
                continue;
            expected_fields += l2_scf_fields(sc);
            for (g = 0; g < l2_scf_fields(sc); g++)
                l2_global_idx_seen[l2_field_index(p, i, g)] = 1;
        }
        l2_msg(msg, sizeof(msg), "6-bit field count cfg=%d got=%d want=%d x=%d",
               modes[mi], model.field_count, expected_fields, 0);
        TASK_CHECK(model.field_count == expected_fields, msg);
        TASK_CHECK(model.field_count > 0, "no 6-bit scale fields read");

        /* production parse of bit allocation, SCFSI and scale factor fields */
        bs_init(&bs, payload, L2_FRAME_BYTES - 4);
        L12_read_scale_info(hdr, &bs, &sci);

        l2_msg(msg, sizeof(msg), "scale-info bit consumption cfg=%d got=%d want=%d x=%d",
               modes[mi], bs.pos, model.pos_after_scf, 0);
        TASK_CHECK(bs.pos == model.pos_after_scf, msg);
        TASK_CHECK(sci.total_bands == L2_NB, "total_bands");
        TASK_CHECK((int)sci.stereo_bands == p->sb_stereo, "stereo_bands boundary");

        for (sb = 0; sb < L2_NB; sb++)
        {
            l2_msg(msg, sizeof(msg), "bitalloc L cfg=%d sb=%d got=%d want=%d",
                   modes[mi], sb, sci.bitalloc[2*sb], p->baL[sb]);
            TASK_CHECK(sci.bitalloc[2*sb] == p->baL[sb], msg);
            if (sb < p->sb_stereo)
                TASK_CHECK(sci.bitalloc[2*sb + 1] == p->baR[sb], "bitalloc R inside boundary");
            else
                TASK_CHECK(sci.bitalloc[2*sb + 1] == 0, "bitalloc R zero past boundary");
            if (p->baL[sb])
                TASK_CHECK(sci.scfcod[2*sb] == p->scfL[sb], "left scfcod");
            if (l2_eff_baR(p, sb))
            {
                l2_msg(msg, sizeof(msg), "right scfcod cfg=%d sb=%d got=%d want=%d",
                       modes[mi], sb, sci.scfcod[2*sb + 1], p->scfR[sb]);
                TASK_CHECK(sci.scfcod[2*sb + 1] == p->scfR[sb], msg);
            }
        }

        /*
         * Direct scale-factor check: every production float
         * sci.scf[3*slot + section] (the multiplier actually applied to each
         * 12-sample moment in L12_apply_scf_384) must equal the double model.
         */
        for (i = 0; i < 2*L2_NB; i++)
        {
            sb = i >> 1; ch = i & 1;
            for (g = 0; g < 3; g++)
            {
                double want = model.scf[ch][sb][g];
                float got = sci.scf[3*i + g];
                l2_msg(msg, sizeof(msg),
                       "scf float cfg=%d ch=%d sb=%d section=%d got=%g want=%g",
                       modes[mi], ch, sb, g, got, want);
                TASK_CHECK(l2_float_close((double)got, want), msg);
            }
        }

        /* joint stereo: shared codeword past the boundary keeps two scale sets */
        if (modes[mi] == L2_MODE_JOINT)
        {
            int shared = -1;
            for (sb = p->sb_stereo; sb < L2_NB; sb++)
                if (p->baL[sb]) { shared = sb; break; }
            TASK_CHECK(shared >= 0, "joint frame needs a shared codeword past boundary");
            TASK_CHECK(p->baR[shared] == 0, "shared band has no right codeword");
            TASK_CHECK(l2_eff_baR(p, shared) == p->baL[shared], "stale right allocation models parser");
            /* the two scale sets must differ: never copy right scale from left */
            for (g = 0; g < 3; g++)
            {
                l2_msg(msg, sizeof(msg), "shared scale sets equal sb=%d g=%d a=%d b=%d",
                       shared, g, 0, 0);
                TASK_CHECK(model.scf[0][shared][g] != model.scf[1][shared][g], msg);
                TASK_CHECK(model.scf[1][shared][g] != 0.0, "right scale set is real");
            }
        }

        /* three granules through the real dequantizer and scale-factor stage */
        {
            int i_pos = 0;
            memset(gr, 0, sizeof(gr));
            for (g = 0; g < 3; g++)
            {
                i_pos += L12_dequantize_granule(gr + i_pos, &bs, &sci, 3);
                l2_msg(msg, sizeof(msg), "granule bit consumption cfg=%d g=%d got=%d want=%d",
                       modes[mi], g, bs.pos, model.pos_after_gr[g]);
                TASK_CHECK(bs.pos == model.pos_after_gr[g], msg);
                TASK_CHECK(i_pos == 12, "granule produced 12 sample slots");
                L12_apply_scf_384(&sci, sci.scf + g, gr);
                l2_compare_granule(p, &model, g, gr);
                memset(gr, 0, sizeof(gr));
                i_pos = 0;
            }
        }

        /* explicit grouped-word integer check on this configuration */
        TASK_CHECK(model.grouped_words > 0, "no grouped codewords parsed");
        TASK_CHECK(model.last_codeword_pos[1] - model.last_codeword_pos[0] > 0,
                   "grouped codeword consumed bits");
    }

    /* the required indices 0, 1, 2, 62, 63 must all appear across the group */
    {
        static const int required[5] = { 0, 1, 2, 62, 63 };
        for (i = 0; i < 5; i++)
        {
            l2_msg(msg, sizeof(msg), "required scale index %d never transmitted x=%d y=%d z=%d",
                   required[i], 0, 0, 0);
            TASK_CHECK(l2_global_idx_seen[required[i]], msg);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* frames group: full mp3dec_decode_frame contract.                    */
/* ------------------------------------------------------------------ */

static int l2_group_frames(void)
{
    static const int modes[3] = { L2_MODE_MONO, L2_MODE_STEREO, L2_MODE_JOINT };
    static const char *names[3] = { "mono", "stereo", "joint" };
    int mi, f, g;
    char msg[192];

    /* continuous mono stream: three identical frames decoded back to back */
    {
        l2_plan_t plan;
        static unsigned char stream[L2_FRAME_BYTES*3];
        mp3dec_t dec;
        short pcm[2304 + 64];
        int off = 0, frames = 0;
        l2_fill_plan(&plan, L2_MODE_MONO);
        for (f = 0; f < 3; f++)
        {
            memset(stream + f*L2_FRAME_BYTES, 0, L2_FRAME_BYTES);
            l2_make_header(&plan, stream + f*L2_FRAME_BYTES);
            l2_build_payload(&plan, stream + f*L2_FRAME_BYTES + 4);
        }
        mp3dec_init(&dec);
        while (off < (int)sizeof(stream))
        {
            mp3dec_frame_info_t info;
            int nz, guard, k;
            short *out = pcm + 32;
            for (k = 0; k < 32; k++) { pcm[k] = (short)0x5555; pcm[32 + 2304 + k] = (short)0x5555; }
            memset(&info, 0, sizeof(info));
            l2_obs_count = 0;
            int z = mp3dec_decode_frame(&dec, stream + off, (int)sizeof(stream) - off, out, &info);
            l2_msg(msg, sizeof(msg), "mono stream frame=%d z=%d fb=%d ch=%d",
                   frames, z, info.frame_bytes, info.channels);
            TASK_CHECK(z == 1152, msg);
            TASK_CHECK(info.frame_bytes == L2_FRAME_BYTES, "mono frame bytes");
            TASK_CHECK(info.channels == 1 && info.layer == 2, "mono format info");
            TASK_CHECK(info.hz == 44100 && info.bitrate_kbps == 192, "mono hz/bitrate");
            nz = 0;
            for (k = 0; k < z*info.channels; k++)
                if (out[k]) nz++;
            TASK_CHECK(nz > 0, "mono PCM is nonzero");
            guard = 1;
            for (k = 0; k < 32; k++)
                if (pcm[k] != (short)0x5555 || pcm[32 + 2304 + k] != (short)0x5555)
                    guard = 0;
            TASK_CHECK(guard, "mono output guard zone untouched");
            off += info.frame_bytes;
            frames++;
            if (frames == 3) break;
        }
        TASK_CHECK(frames == 3 && off == (int)sizeof(stream), "continuous mono consumed exactly");
    }

    /* mono, normal stereo and joint stereo, each with a matching follow frame */
    for (mi = 0; mi < 3; mi++)
    {
        l2_plan_t plan;
        l2_model_t model;
        static unsigned char two[L2_FRAME_BYTES*2];
        unsigned char *frame = two;
        mp3dec_t dec;
        short pcm[2304 + 64];
        short *out = pcm + 32;
        mp3dec_frame_info_t info;
        int nz = 0, guard = 1, k, z;

        l2_fill_plan(&plan, modes[mi]);
        memset(two, 0, sizeof(two));
        l2_make_header(&plan, frame);
        l2_build_payload(&plan, frame + 4);
        memcpy(frame + L2_FRAME_BYTES, frame, L2_FRAME_BYTES);
        l2_model_run(&plan, frame + 4, &model);

        for (k = 0; k < 32; k++) { pcm[k] = (short)0x6666; pcm[32 + 2304 + k] = (short)0x6666; }
        mp3dec_init(&dec);
        l2_obs_count = 0;
        z = mp3dec_decode_frame(&dec, two, (int)sizeof(two), out, &info);

        l2_msg(msg, sizeof(msg), "%s frame z=%d fb=%d ch=%d layer=%d",
               names[mi], z, info.frame_bytes, info.channels, info.layer);
        TASK_CHECK(z == 1152, msg);
        TASK_CHECK(info.frame_bytes == L2_FRAME_BYTES, "frame bytes consumed");
        TASK_CHECK(info.layer == 2, "reported layer");
        TASK_CHECK(info.channels == (modes[mi] == L2_MODE_MONO ? 1 : 2), "channel count");
        TASK_CHECK(info.hz == 44100 && info.bitrate_kbps == 192, "format info");
        for (k = 0; k < z*info.channels; k++)
            if (out[k]) nz++;
        TASK_CHECK(nz > 0, "nonzero PCM");
        for (k = 0; k < 32; k++)
            if (pcm[k] != (short)0x6666 || pcm[32 + 2304 + k] != (short)0x6666)
                guard = 0;
        TASK_CHECK(guard, "output guard zone untouched");

        /* read-only observation of the three pre-synthesis subband sets */
        TASK_CHECK(l2_obs_count == 3, "three observed granules");
        TASK_CHECK(l2_obs_total == L2_NB, "observed total bands");
        TASK_CHECK(l2_obs_stereo == plan.sb_stereo, "observed stereo boundary");
        for (g = 0; g < 3; g++)
            l2_compare_granule(&plan, &model, g, l2_obs_grbuf[g]);

        /* both grouped and ordinary allocations and all SCFSI modes present */
        {
            int has_group = 0, has_plain = 0, seen[4] = { 0,0,0,0 }, sb;
            for (sb = 0; sb < L2_NB; sb++)
            {
                if (plan.baL[sb] >= 17) has_group = 1;
                if (plan.baL[sb] > 0 && plan.baL[sb] < 17) has_plain = 1;
                if (plan.baL[sb]) seen[plan.scfL[sb]] = 1;
                if (l2_eff_baR(&plan, sb)) seen[plan.scfR[sb]] = 1;
            }
            TASK_CHECK(has_group && has_plain, "frame mixes grouped and ordinary quantization");
            for (k = 0; k < 4; k++)
            {
                l2_msg(msg, sizeof(msg), "frame scfcod %d missing cfg=%d a=%d b=%d",
                       k, modes[mi], 0, 0);
                TASK_CHECK(seen[k], msg);
            }
        }
    }
    return 1;
}

int task_group(const char *group)
{
    if (!strcmp(group, "digits"))
        return l2_group_digits();
    if (!strcmp(group, "scfsi"))
        return l2_group_scfsi();
    if (!strcmp(group, "frames"))
        return l2_group_frames();
    TASK_CHECK(0, "unknown group");
    return 0;
}
