/*
 * Independent selection, bitstream syntax and streaming tests for optional
 * VisualOn AAC PNS.  Everything here is test-only; it links against the
 * production archive and also reads read-only encoder/spectrum state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "task_support/check.h"
#include "voAAC.h"
#include "cmnMemory.h"
#include "aacenc_core.h"
#include "aac_rom.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*----------------------------- fixed generators ----------------------------*/

typedef struct { uint32_t state; } Lcg;

static int16_t lcg_noise(Lcg *g, int divisor)
{
    uint32_t x = g->state;
    x = x * 1664525u + 1013904223u;
    g->state = x;
    return (int16_t)(((int32_t)(x >> 16) - 32768) / divisor);
}

static int16_t tone_sample(uint64_t n, int rate, int hz, int amp)
{
    /* 4096-entry integer phase sine, generated once from the C math library. */
    static int table[4096];
    static int ready = 0;
    if (!ready) {
        for (int i = 0; i < 4096; ++i)
            table[i] = (int)lround(32767.0 * sin(2.0 * M_PI * i / 4096.0));
        ready = 1;
    }
    uint64_t phase = (n * (uint64_t)hz * 4096u / (uint64_t)rate) & 4095u;
    return (int16_t)(table[phase] * amp / 32768);
}

enum { SIG_NOISE, SIG_TONE, SIG_TRANSIENT, SIG_LEFT_NOISE, SIG_RIGHT_TONE };

static void gen_frame(int kind, int rate, int channels, unsigned frame,
                      int16_t *pcm)
{
    Lcg glocal = { 0x1234abcdU ^ (frame * 2654435761u) ^ 0x9e3779b9u };
    Lcg *g = &glocal;
    uint64_t base = (uint64_t)frame * 1024u;

    for (int i = 0; i < 1024; ++i) {
        uint64_t n = base + (unsigned)i;
        int16_t noise = lcg_noise(g, 4);
        int16_t tone = tone_sample(n, rate, rate == 44100 ? 7000 : 7500, 11000);
        int impulse_pos = (int)((frame * 173 + 97) % 1024);
        int d = abs(i - impulse_pos);
        int transient = (d < 40) ? (int)(26000.0 * exp(-d / 5.0) *
                                        ((i & 1) ? 1 : -1)) : noise / 6;
        int l = 0, r = 0;

        switch (kind) {
        case SIG_NOISE:
            l = noise;
            r = noise;
            break;
        case SIG_TONE:
            l = tone;
            r = l;
            break;
        case SIG_TRANSIENT:
            l = transient + noise / 10;
            r = l;
            break;
        case SIG_LEFT_NOISE:
            l = noise;
            r = tone + noise / 24;
            break;
        case SIG_RIGHT_TONE:
            l = tone + noise / 24;
            r = noise;
            break;
        }

        pcm[i * channels + 0] = (int16_t)l;
        if (channels == 2)
            pcm[i * channels + 1] = (int16_t)r;
    }
}

/*------------------------------- encoder helper ----------------------------*/

typedef struct {
    VO_AUDIO_CODECAPI api;
    VO_HANDLE h;
    VO_MEM_OPERATOR mem;
    int rate, channels;
} Enc;

static int enc_init(Enc *e, int rate, int channels, int adts, int pns)
{
    AACENC_PARAM p;
    VO_CODEC_INIT_USERDATA u;
    VO_S32 on = pns ? 1 : 0;

    memset(e, 0, sizeof(*e));
    e->rate = rate;
    e->channels = channels;
    e->mem.Alloc = cmnMemAlloc;
    e->mem.Copy = cmnMemCopy;
    e->mem.Free = cmnMemFree;
    e->mem.Set = cmnMemSet;
    e->mem.Check = cmnMemCheck;
    memset(&u, 0, sizeof(u));
    u.memflag = VO_IMF_USERMEMOPERATOR;
    u.memData = &e->mem;

    if (voGetAACEncAPI(&e->api)) return -1;
    if (e->api.Init(&e->h, VO_AUDIO_CodingAAC, &u)) return -2;
    memset(&p, 0, sizeof(p));
    p.sampleRate = rate;
    p.bitRate = channels == 1 ? 96000 : 128000;
    p.nChannels = (short)channels;
    p.adtsUsed = (short)adts;
    if (e->api.SetParam(e->h, VO_PID_AAC_ENCPARAM, &p)) return -3;
    if (pns && e->api.SetParam(e->h, VO_PID_AAC_PNS, &on)) return -4;
    return 0;
}

static void enc_destroy(Enc *e)
{
    if (e->h) e->api.Uninit(e->h);
    memset(e, 0, sizeof(*e));
}

static int enc_set_pns(Enc *e, VO_S32 v)
{
    return (int)e->api.SetParam(e->h, VO_PID_AAC_PNS, &v);
}

static int enc_get_pns(Enc *e)
{
    VO_S32 v = -123;
    if (e->api.GetParam(e->h, VO_PID_AAC_PNS, &v)) return -1;
    return (int)v;
}

static int enc_frame(Enc *e, const int16_t *pcm, uint8_t *out, size_t cap,
                     size_t *out_len)
{
    VO_CODECBUFFER in, ob;
    VO_AUDIO_OUTPUTINFO info;
    VO_U32 r;

    memset(&in, 0, sizeof(in));
    memset(&ob, 0, sizeof(ob));
    memset(&info, 0, sizeof(info));
    in.Buffer = (VO_PBYTE)(void *)pcm;
    in.Length = 1024u * e->channels * sizeof(int16_t);
    ob.Buffer = out;
    ob.Length = (VO_U32)cap;
    r = e->api.SetInputData(e->h, &in);
    if (r) return (int)r;
    r = e->api.GetOutputData(e->h, &ob, &info);
    if (r) return (int)r;
    *out_len = ob.Length;
    return 0;
}

static AAC_ENCODER *enc_core(Enc *e) { return (AAC_ENCODER *)e->h; }

/*-------------------------------- bit reader -------------------------------*/

typedef struct {
    const uint8_t *data;
    size_t bitpos, nbits;
    int error;
} BR;

static void br_init(BR *b, const uint8_t *data, size_t byte_len,
                    size_t bit_offset)
{
    b->data = data;
    b->nbits = byte_len * 8;
    b->bitpos = bit_offset;
    b->error = 0;
}

static unsigned br_get(BR *b, int n)
{
    unsigned v = 0;
    if (n < 0 || n > 25 || b->bitpos + (size_t)n > b->nbits) {
        b->error = 1;
        return 0;
    }
    while (n--) {
        size_t byte = b->bitpos >> 3;
        int bit = 7 - (int)(b->bitpos & 7);
        v = (v << 1) | ((b->data[byte] >> bit) & 1u);
        ++b->bitpos;
    }
    return v;
}

static unsigned br_peek(BR *b, int n)
{
    size_t save = b->bitpos;
    int err = b->error;
    unsigned v = br_get(b, n);
    b->bitpos = save;
    b->error = err;
    return v;
}

static void br_align_zero(BR *b)
{
    while (b->bitpos & 7u) {
        if (br_get(b, 1) != 0) b->error = 1;
    }
}

static int sf_delta_decode(BR *b)
{
    int idx;
    for (idx = 0; idx < 121; ++idx) {
        int len = (int)huff_ltabscf[idx];
        uint32_t cw = huff_ctabscf[idx];
        if (len <= 0 || len > 25) continue;
        if (br_peek(b, len) == (cw & ((1u << len) - 1u))) {
            (void)br_get(b, len);
            return idx - 60;
        }
    }
    b->error = 1;
    return 0;
}

/*------------------------------ AAC frame parser ----------------------------*/

typedef struct {
    int global_gain;
    int block_type, window_shape, max_sfb, group_mask;
    int num_groups;
    int group_len[8];
    int sfb_stride;
    int total_sfb;
    int cb[MAX_GROUPED_SFB];
    int normal_offset[MAX_GROUPED_SFB];
    int pns_offset[MAX_GROUPED_SFB];
    int pns_count;
    int pns_first_raw;
    int tns_present;
    size_t spectral_start, spectral_end;
} ParsedCh;

typedef struct {
    int id, tag;
    int common_window;
    int ms_digest;
    int ms_mask[MAX_GROUPED_SFB];
    int channels;
    ParsedCh ch[2];
    size_t end_bit;
} ParsedFrame;

static void parse_ics_info(BR *br, ParsedCh *c)
{
    int g, bit;
    (void)br_get(br, 1); /* ics reserved */
    c->block_type = (int)br_get(br, 2);
    c->window_shape = (int)br_get(br, 1);
    if (c->block_type == SHORT_WINDOW) {
        int pos = 0;
        c->max_sfb = (int)br_get(br, 4);
        c->group_mask = (int)br_get(br, 7);
        c->group_len[0] = 1;
        c->num_groups = 1;
        for (bit = 6; bit >= 0; --bit) {
            if ((c->group_mask >> bit) & 1)
                c->group_len[c->num_groups - 1]++;
            else {
                c->num_groups++;
                c->group_len[c->num_groups - 1] = 1;
            }
        }
        for (g = 0; g < c->num_groups; ++g) pos += c->group_len[g];
        if (pos != 8) br->error = 1;
    } else {
        c->max_sfb = (int)br_get(br, 6);
        if (br_get(br, 1)) { /* predictor_data_present: not produced here */
            br->error = 1;
        }
        c->group_mask = 0;
        c->num_groups = 1;
        c->group_len[0] = 1;
    }
    c->sfb_stride = c->max_sfb;
    c->total_sfb = c->num_groups * c->max_sfb;
    if (c->total_sfb > MAX_GROUPED_SFB) br->error = 1;
}

static void parse_sections(BR *br, ParsedCh *c)
{
    int escape = c->block_type == SHORT_WINDOW ? 7 : 31;
    int bits = c->block_type == SHORT_WINDOW ? 3 : 5;

    for (int g = 0; g < c->num_groups && !br->error; ++g) {
        int group_pos = 0;
        while (group_pos < c->max_sfb && !br->error) {
            int cb = (int)br_get(br, 4);
            int cnt = 0, inc;
            do {
                inc = (int)br_get(br, bits);
                cnt += inc;
            } while (inc == escape && !br->error);
            for (int k = 0; k < cnt && group_pos < c->max_sfb; ++k) {
                int idx = g * c->sfb_stride + group_pos + k;
                if (idx < MAX_GROUPED_SFB) c->cb[idx] = cb;
            }
            group_pos += cnt;
            if (cnt <= 0 || group_pos > c->max_sfb) br->error = 1;
        }
    }
}

static void parse_scalechains(BR *br, ParsedCh *c)
{
    int normal = c->global_gain;
    int noise = c->global_gain - 90;
    int first_noise = 1;

    for (int g = 0; g < c->num_groups; ++g) {
      for (int s = 0; s < c->max_sfb; ++s) {
        int i = g * c->sfb_stride + s;
        int cb = c->cb[i];
        c->normal_offset[i] = normal;
        c->pns_offset[i] = noise;
        if (cb == CODE_BOOK_ZERO_NO || cb == CODE_BOOK_RES_NO)
            continue;
        if (cb == CODE_BOOK_PNS_NO) {
            if (first_noise) {
                int raw = (int)br_get(br, 9);
                c->pns_first_raw = raw;
                noise += raw - 256;
                first_noise = 0;
            } else {
                noise += sf_delta_decode(br);
            }
            c->pns_offset[i] = noise;
            c->pns_count++;
        } else {
            normal += sf_delta_decode(br);
            c->normal_offset[i] = normal;
        }
      }
    }
}

/*
 * Skip real spectral Huffman codes.  The quantized values and SFB widths are
 * read-only observations; all codewords/sign/escape bits are consumed from the
 * actual AAC payload.
 */
static void expect_code(BR *br, uint32_t code, int len)
{
    unsigned got;
    if (len <= 0 || br->bitpos + (size_t)len > br->nbits) { br->error = 1; return; }
    got = br_get(br, len);
    if (got != (code & ((1u << len) - 1u)))
        br->error = 1;
}

static int qabs(int x) { return x < 0 ? -x : x; }

static void skip_spectral_band(BR *br, int cb, const int16_t *q, int width)
{
    int i;
    if (cb == 0 || cb == 13) return;

    for (i = 0; i < width;) {
        int sl = 0, sbits = 0;
        if (cb == 1 || cb == 2) {
            int a = q[i] + 1, b = q[i+1] + 1, c = q[i+2] + 1, d = q[i+3] + 1;
            if (cb == 1) expect_code(br, huff_ctab1[a][b][c][d],
                                     huff_ltab1_2[a][b][c][d] >> 8);
            else expect_code(br, huff_ctab2[a][b][c][d],
                             huff_ltab1_2[a][b][c][d] & 255);
            i += 4;
        } else if (cb == 3 || cb == 4) {
            int v[4] = { qabs(q[i]), qabs(q[i+1]), qabs(q[i+2]), qabs(q[i+3]) };
            for (int k = 0; k < 4; ++k) if (q[i+k]) { sl++; sbits = (sbits << 1) | (q[i+k] < 0); }
            if (cb == 3) expect_code(br, huff_ctab3[v[0]][v[1]][v[2]][v[3]],
                                     huff_ltab3_4[v[0]][v[1]][v[2]][v[3]] >> 8);
            else expect_code(br, huff_ctab4[v[0]][v[1]][v[2]][v[3]],
                             huff_ltab3_4[v[0]][v[1]][v[2]][v[3]] & 255);
            for (int k = sl - 1; k >= 0; --k) (void)br_get(br, 1);
            i += 4;
        } else if (cb == 5 || cb == 6) {
            int a = q[i] + 4, b = q[i+1] + 4;
            if (cb == 5) expect_code(br, huff_ctab5[a][b], huff_ltab5_6[a][b] >> 8);
            else expect_code(br, huff_ctab6[a][b], huff_ltab5_6[a][b] & 255);
            i += 2;
        } else if (cb >= 7 && cb <= 10) {
            int table = cb;
            int a = qabs(q[i]), b = qabs(q[i+1]);
            if (table == 7) expect_code(br, huff_ctab7[a][b], huff_ltab7_8[a][b] >> 8);
            else if (table == 8) expect_code(br, huff_ctab8[a][b], huff_ltab7_8[a][b] & 255);
            else if (table == 9) expect_code(br, huff_ctab9[a][b], huff_ltab9_10[a][b] >> 8);
            else if (table == 10)
                expect_code(br, huff_ctab10[a][b], huff_ltab9_10[a][b] & 255);
            for (int k = 0; k < 2; ++k) if (q[i+k]) (void)br_get(br, 1);
            i += 2;
        } else if (cb == 11) {
            int a = qabs(q[i]), b = qabs(q[i+1]);
            int aa = a < 16 ? a : 16, bb = b < 16 ? b : 16;
            expect_code(br, huff_ctab11[aa][bb],
                        huff_ltab11[aa][bb]);
            for (int k = 0; k < 2; ++k) if (q[i+k]) (void)br_get(br, 1);
            for (int pair = 0; pair < 2; ++pair) {
                int v = pair == 0 ? a : b;
                if (v >= 16) {
                    int n = 0;
                    while (br_get(br, 1) == 1 && !br->error) n++;
                    for (int k = 0; k < n + 4; ++k) (void)br_get(br, 1);
                }
            }
            i += 2;
        } else {
            br->error = 1;
            return;
        }
    }
}

static void parse_channel(BR *br, ParsedCh *c, int common,
                          const PSY_OUT_CHANNEL *po,
                          const QC_OUT_CHANNEL *qc)
{
    if (!common)
        memset(c, 0, sizeof(*c));
    c->global_gain = (int)br_get(br, 8);
    if (!common) parse_ics_info(br, c);
    if (po && po->sfbPerGroup > 0) {
        c->sfb_stride = po->sfbPerGroup;
        c->total_sfb = c->num_groups * c->sfb_stride;
    }
    parse_sections(br, c);
    parse_scalechains(br, c);
    if (br_get(br, 1)) { /* pulse data */
        br->error = 1;
        return;
    }
    c->tns_present = (int)br_get(br, 1);
    if (c->tns_present) {
        /* PNS test streams require TNS off.  Other syntax cases do not call
           the full spectral-boundary parser on TNS frames. */
        br->error = 1;
        return;
    }
    (void)br_get(br, 1); /* gain control */
    c->spectral_start = br->bitpos;

    if (po && qc) {
        for (int sfb = 0; sfb < c->total_sfb && !br->error; ++sfb) {
            int start = po->sfbOffsets[sfb];
            int width = po->sfbOffsets[sfb + 1] - start;
            skip_spectral_band(br, c->cb[sfb], qc->quantSpec + start, width);
        }
    }
    c->spectral_end = br->bitpos;
}

static void parse_ms(BR *br, ParsedFrame *f)
{
    f->ms_digest = (int)br_get(br, 2);
    if (f->ms_digest == MS_SOME) {
        ParsedCh *c = &f->ch[0];
        for (int g = 0; g < c->num_groups; ++g)
            for (int sfb = 0; sfb < c->max_sfb; ++sfb) {
                int idx = g * c->sfb_stride + sfb;
                f->ms_mask[idx] = (int)br_get(br, 1);
            }
    } else if (f->ms_digest == MS_ALL) {
        ParsedCh *c = &f->ch[0];
        for (int i = 0; i < c->total_sfb; ++i) f->ms_mask[i] = 1;
    }
}

static int parse_frame(const uint8_t *data, size_t len, int adts,
                       AAC_ENCODER *core, ParsedFrame *f)
{
    BR br;
    size_t off = 0;
    int idx[2] = {0, 1};
    memset(f, 0, sizeof(*f));

    if (adts) {
        unsigned hlen;
        if (len < 7 || data[0] != 0xff || (data[1] & 0xf0) != 0xf0) return -1;
        hlen = (data[1] & 1) ? 7u : 9u;
        off = hlen * 8;
    }
    br_init(&br, data, len, off);
    f->id = (int)br_get(&br, 3);
    f->tag = (int)br_get(&br, 4);
    if (f->id == ID_SCE) {
        f->channels = 1;
        f->common_window = 0;
        idx[0] = core->elInfo.ChannelIndex[0];
        parse_channel(&br, &f->ch[0], 0,
                      &core->psyOut.psyOutChannel[idx[0]],
                      &core->qcOut.qcChannel[idx[0]]);
    } else if (f->id == ID_CPE) {
        f->channels = 2;
        f->common_window = (int)br_get(&br, 1);
        idx[0] = core->elInfo.ChannelIndex[0];
        idx[1] = core->elInfo.ChannelIndex[1];
        parse_ics_info(&br, &f->ch[0]);
        if (core->psyOut.psyOutChannel[idx[0]].sfbPerGroup > 0) {
            f->ch[0].sfb_stride = core->psyOut.psyOutChannel[idx[0]].sfbPerGroup;
            f->ch[0].total_sfb = f->ch[0].num_groups * f->ch[0].sfb_stride;
        }
        f->ch[1] = f->ch[0];
        parse_ms(&br, f);
        parse_channel(&br, &f->ch[0], 1,
                      &core->psyOut.psyOutChannel[idx[0]],
                      &core->qcOut.qcChannel[idx[0]]);
        parse_channel(&br, &f->ch[1], 1,
                      &core->psyOut.psyOutChannel[idx[1]],
                      &core->qcOut.qcChannel[idx[1]]);
    } else {
        return -2;
    }

    /* Fill elements and ID_END follow the channel spectral payload. */
    for (;;) {
        int id = (int)br_get(&br, 3);
        f->end_bit = br.bitpos;
        if (br.error) return -3;
        if (id == ID_END) break;
        if (id != ID_FIL) return -4;
        int cnt = (int)br_get(&br, 4);
        if (cnt == 15) {
            int esc = (int)br_get(&br, 8);
            cnt = 14 + esc;
        }
        for (int i = 0; i < cnt; ++i) (void)br_get(&br, 8);
    }
    br_align_zero(&br);
    f->end_bit = br.bitpos;
    if (br.error || f->end_bit != len * 8) return -5;
    return 0;
}

/*------------------------ read-only spectrum observation --------------------*/

typedef struct {
    int valid;
    int physical_windows;
    int lines;
    int all_noise_like;
    double average_energy;
} BandObs;

static uint64_t obs_abs(int32_t x)
{
    uint32_t u = (uint32_t)x;
    return (u & 0x80000000u) ? 0x100000000ULL - (uint64_t)u : (uint64_t)u;
}

static int criterion_on_window(const int32_t *x, int n, double *energy_out)
{
    unsigned __int128 e = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        uint64_t a = obs_abs(x[i]);
        unsigned __int128 sq = (unsigned __int128)a * a;
        e += sq;
        if (sq > mx) mx = sq;
    }
    if (energy_out) *energy_out = (double)e;
    return n >= 8 && e > 0 && (unsigned __int128)n * mx <= (unsigned __int128)8 * e;
}

static BandObs observe_band(AAC_ENCODER *core, int ch, int idx)
{
    BandObs o;
    PSY_OUT_CHANNEL *po = &core->psyOut.psyOutChannel[ch];
    memset(&o, 0, sizeof(o));

    if (po->windowSequence != SHORT_WINDOW) {
        int start = po->sfbOffsets[idx];
        int n = po->sfbOffsets[idx + 1] - start;
        double e;
        o.valid = 1;
        o.physical_windows = 1;
        o.lines = n;
        o.all_noise_like = criterion_on_window(po->pnsObsSpectrum + start, n, &e);
        o.average_energy = e;
        return o;
    }

    {
        int sfb_per_group = po->sfbPerGroup;
        int g = idx / sfb_per_group;
        int sfb = idx - g * sfb_per_group;
        int len = core->psyKernel.psyData[ch].blockSwitchingControl.groupLen[g];
        const Word16 *so = core->psyKernel.psyConfShort.sfbOffset;
        int n = so[sfb + 1] - so[sfb];
        double sum = 0.0;
        int all = len > 0;
        int window_start = 0;

        for (int gg = 0; gg < g; ++gg)
            window_start += core->psyKernel.psyData[ch].blockSwitchingControl.groupLen[gg];

        o.valid = g < core->psyKernel.psyData[ch].blockSwitchingControl.noOfGroups;
        o.physical_windows = len;
        o.lines = n;
        for (int w = 0; w < len; ++w) {
            int win = window_start + w;
            double e = 0.0;
            const int32_t *x = po->pnsObsSpectrum + win * FRAME_LEN_SHORT + so[sfb];
            int ok = criterion_on_window(x, n, &e);
            if (!ok) all = 0;
            sum += e;
        }
        o.all_noise_like = all;
        o.average_energy = len ? sum / len : 0.0;
        return o;
    }
}

static int band_lower_hz(AAC_ENCODER *core, int idx)
{
    PSY_OUT_CHANNEL *po = &core->psyOut.psyOutChannel[0];
    int lines = po->windowSequence == SHORT_WINDOW ? 128 : 1024;
    int off;
    if (po->windowSequence == SHORT_WINDOW) {
        int s = idx % po->sfbPerGroup;
        off = core->psyKernel.psyConfShort.sfbOffset[s];
    } else {
        off = po->sfbOffsets[idx];
    }
    return (int)(((int64_t)off * core->config.sampleRate) / (2 * lines));
}

static int check_pns_energy(AAC_ENCODER *core, const ParsedFrame *pf, int ch,
                            int idx)
{
    PSY_OUT_CHANNEL *po = &core->psyOut.psyOutChannel[ch];
    BandObs o = observe_band(core, ch, idx);
    double target, decoded, db;

    if (!o.valid || !o.all_noise_like || o.average_energy <= 0.0) return 0;
    target = o.average_energy * ldexp(1.0, 2 * ((int)po->mdctScale - 15));
    decoded = pow(2.0, pf->ch[ch].pns_offset[idx] / 2.0);
    db = 10.0 * log10(decoded / target);
    return fabs(db) <= 1.5;
}

/*------------------------------- byte blobs --------------------------------*/

typedef struct { uint8_t *data; size_t len; } Blob;
typedef struct { Blob *v; size_t n, cap; } Blobs;

static void blobs_push(Blobs *b, const uint8_t *p, size_t n)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->v = realloc(b->v, b->cap * sizeof(*b->v));
    }
    b->v[b->n].data = malloc(n);
    b->v[b->n].len = n;
    memcpy(b->v[b->n].data, p, n);
    b->n++;
}

static void blobs_free(Blobs *b)
{
    for (size_t i = 0; i < b->n; ++i) free(b->v[i].data);
    free(b->v);
    memset(b, 0, sizeof(*b));
}

static int blobs_equal(const Blobs *a, const Blobs *b)
{
    if (a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; ++i) {
        if (a->v[i].len != b->v[i].len ||
            memcmp(a->v[i].data, b->v[i].data, a->v[i].len))
            return 0;
    }
    return 1;
}

static int drain_outputs(Enc *e, Blobs *out)
{
    for (;;) {
        uint8_t buf[8192];
        VO_CODECBUFFER ob;
        VO_AUDIO_OUTPUTINFO info;
        VO_U32 r;
        memset(&ob, 0, sizeof(ob));
        memset(&info, 0, sizeof(info));
        ob.Buffer = buf;
        ob.Length = sizeof(buf);
        r = e->api.GetOutputData(e->h, &ob, &info);
        if (r == VO_ERR_INPUT_BUFFER_SMALL) return 0;
        if (r != VO_ERR_NONE) return (int)r;
        blobs_push(out, buf, ob.Length);
    }
}

static int encode_whole_sequence(int sig, int frames, int pns, Blobs *out)
{
    Enc e;
    int16_t *pcm = calloc(1024 * 2, sizeof(int16_t));
    if (enc_init(&e, 48000, 1, 1, pns)) return -1;
    for (int f = 0; f < frames; ++f) {
        uint8_t buf[8192]; size_t len = 0;
        gen_frame(sig, 48000, 1, f, pcm);
        if (enc_frame(&e, pcm, buf, sizeof(buf), &len)) return -2;
        blobs_push(out, buf, len);
    }
    enc_destroy(&e);
    free(pcm);
    return 0;
}

static int encode_chunked_sequence(int sig, int frames, Blobs *out)
{
    Enc e;
    const size_t frame_bytes = 1024 * sizeof(int16_t);
    size_t total = (size_t)frames * frame_bytes;
    int16_t *pcm = malloc(total);
    static const size_t chunks[] = {
        2, 1022, 1024, 510, 1538, 4, 2044, 768, 1278, 2048, 6, 1018
    };
    size_t off = 0, ci = 0;

    if (enc_init(&e, 48000, 1, 1, 1)) return -1;
    for (int f = 0; f < frames; ++f)
        gen_frame(sig, 48000, 1, f, pcm + f * 1024);

    while (off < total) {
        size_t n = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
        VO_CODECBUFFER in;
        if (n > total - off) n = total - off;
        memset(&in, 0, sizeof(in));
        in.Buffer = (VO_PBYTE)((uint8_t *)pcm + off);
        in.Length = (VO_U32)n;
        if (e.api.SetInputData(e.h, &in)) return -2;
        if (drain_outputs(&e, out)) return -3;
        off += n;
    }
    enc_destroy(&e);
    free(pcm);
    return 0;
}

/*--------------------------------- cases -----------------------------------*/

static int case_param_validation_timing(void)
{
    Enc e;
    VO_S32 on = 1, bad = 7, got;
    int16_t pcm[1024];
    uint8_t buf[8192];
    size_t len;
    VO_CODECBUFFER in, ob;
    VO_AUDIO_OUTPUTINFO info;

    TASK_CHECK(enc_init(&e, 48000, 1, 1, 0) == 0);
    TASK_CHECK(enc_get_pns(&e) == 0);
    TASK_CHECK(e.api.SetParam(NULL, VO_PID_AAC_PNS, &on) == VO_ERR_INVALID_ARG);
    TASK_CHECK(e.api.GetParam(NULL, VO_PID_AAC_PNS, &got) == VO_ERR_INVALID_ARG);
    TASK_CHECK(e.api.SetParam(e.h, VO_PID_AAC_PNS, NULL) == VO_ERR_INVALID_ARG);
    TASK_CHECK(e.api.GetParam(e.h, VO_PID_AAC_PNS, NULL) == VO_ERR_INVALID_ARG);
    TASK_CHECK(e.api.SetParam(e.h, VO_PID_AAC_PNS, &bad) == VO_ERR_INVALID_ARG);
    TASK_CHECK(enc_get_pns(&e) == 0);

    gen_frame(SIG_NOISE, 48000, 1, 0, pcm);

    /* Feed exactly half a PCM frame.  The switch must not become effective. */
    memset(&in, 0, sizeof(in));
    in.Buffer = (VO_PBYTE)pcm;
    in.Length = 1024;
    TASK_CHECK(e.api.SetInputData(e.h, &in) == 0);
    memset(&ob, 0, sizeof(ob)); ob.Buffer = buf; ob.Length = sizeof(buf);
    memset(&info, 0, sizeof(info));
    TASK_CHECK(e.api.GetOutputData(e.h, &ob, &info) == VO_ERR_INPUT_BUFFER_SMALL);
    TASK_CHECK(enc_set_pns(&e, 1) == 0);
    TASK_CHECK(enc_get_pns(&e) == 0);

    memset(&in, 0, sizeof(in));
    in.Buffer = (VO_PBYTE)(pcm + 512);
    in.Length = 1024;
    TASK_CHECK(e.api.SetInputData(e.h, &in) == 0);

    memset(&ob, 0, sizeof(ob)); ob.Buffer = buf; ob.Length = sizeof(buf);
    TASK_CHECK(e.api.GetOutputData(e.h, &ob, &info) == VO_ERR_NONE);
    TASK_CHECK(ob.Length > 7);
    TASK_CHECK(enc_get_pns(&e) == 1);

    /* Last legal request wins; invalid requests do not change it. */
    TASK_CHECK(enc_set_pns(&e, 0) == 0);
    TASK_CHECK(enc_set_pns(&e, 1) == 0);
    TASK_CHECK(enc_set_pns(&e, 0) == 0);
    bad = 9;
    TASK_CHECK(e.api.SetParam(e.h, VO_PID_AAC_PNS, &bad) == VO_ERR_INVALID_ARG);
    TASK_CHECK(enc_get_pns(&e) == 1); /* still effective until next frame */
    gen_frame(SIG_NOISE, 48000, 1, 1, pcm);
    TASK_CHECK(enc_frame(&e, pcm, buf, sizeof(buf), &len) == 0);
    TASK_CHECK(enc_get_pns(&e) == 0);
    {
        ParsedFrame pf;
        TASK_CHECK(parse_frame(buf, len, 1, enc_core(&e), &pf) == 0);
        TASK_CHECK(pf.ch[0].pns_count == 0);
    }
    enc_destroy(&e);
    return 0;
}


static int case_selection_noise_bands(void)
{
    Enc e;
    int16_t pcm[1024];
    uint8_t buf[8192];
    size_t len;
    int saw = 0, energy_checks = 0, saw_long = 0, saw_short = 0;
    int saw_nonuniform = 0, saw_section_escape = 0;

    TASK_CHECK(enc_init(&e, 48000, 1, 1, 1) == 0);
    for (int f = 0; f < 48; ++f) {
        ParsedFrame pf;
        PSY_OUT_CHANNEL *po;
        int sig = f < 18 ? SIG_NOISE : SIG_TRANSIENT;
        gen_frame(sig, 48000, 1, (unsigned)f, pcm);
        TASK_CHECK(enc_frame(&e, pcm, buf, sizeof(buf), &len) == 0);
        TASK_CHECK(parse_frame(buf, len, 1, enc_core(&e), &pf) == 0);
        po = &enc_core(&e)->psyOut.psyOutChannel[0];
        if (po->windowSequence == SHORT_WINDOW) saw_short = 1;
        else saw_long = 1;
        if (po->windowSequence == SHORT_WINDOW) {
            int ng = enc_core(&e)->psyKernel.psyData[0].blockSwitchingControl.noOfGroups;
            if (ng >= 2 && ng <= 4) {
                int lens[8], different = 0;
                for (int g = 0; g < ng; ++g) {
                    lens[g] = enc_core(&e)->psyKernel.psyData[0].blockSwitchingControl.groupLen[g];
                    if (g && lens[g] != lens[0]) different = 1;
                }
                if (different) saw_nonuniform = 1;
            }
        }
        for (int s = 0; s < enc_core(&e)->qcOut.qcChannel[0].sectionData.noOfSections; ++s) {
            SECTION_INFO *si = &enc_core(&e)->qcOut.qcChannel[0].sectionData.sectionInfo[s];
            if (si->codeBook == 13 &&
                ((po->windowSequence == SHORT_WINDOW && si->sfbCnt >= 7) ||
                 (po->windowSequence != SHORT_WINDOW && si->sfbCnt >= 31)))
                saw_section_escape = 1;
        }

        for (int idx = 0; idx < po->sfbCnt; ++idx) {
            BandObs o;
            if (!po->pnsFlag[idx]) continue;
            saw++;
            o = observe_band(enc_core(&e), 0, idx);
            TASK_CHECK(o.valid);
            TASK_CHECK(band_lower_hz(enc_core(&e), idx) >= 4000);
            TASK_CHECK(o.lines >= 8);
            TASK_CHECK(o.all_noise_like);
            if (idx < pf.ch[0].total_sfb) {
                TASK_CHECK(pf.ch[0].cb[idx] == 13);
                TASK_CHECK(pf.ch[0].pns_offset[idx] == po->pnsNoiseExp[idx]);
                TASK_CHECK(check_pns_energy(enc_core(&e), &pf, 0, idx));
                energy_checks++;
            }
        }
    }
    TASK_CHECK(saw > 20);
    TASK_CHECK(energy_checks > 20);
    TASK_CHECK(saw_long);
    TASK_CHECK(saw_short);
    TASK_CHECK(saw_nonuniform);
    /* Broadband frames should produce a maximal short-group run often enough
       to exercise the seven-SFB escape encoding. */
    TASK_CHECK(saw_section_escape);
    enc_destroy(&e);
    return 0;
}

static int case_selection_tone_rejected(void)
{
    Enc e;
    int16_t pcm[1024];
    uint8_t buf[8192];
    size_t len;
    int checked_band = 0;

    TASK_CHECK(enc_init(&e, 48000, 1, 1, 1) == 0);
    for (int f = 0; f < 12; ++f) {
        PSY_OUT_CHANNEL *po;
        gen_frame(SIG_TONE, 48000, 1, (unsigned)f, pcm);
        TASK_CHECK(enc_frame(&e, pcm, buf, sizeof(buf), &len) == 0);
        po = &enc_core(&e)->psyOut.psyOutChannel[0];
        if (po->windowSequence == SHORT_WINDOW) continue;
        for (int idx = 0; idx + 1 <= po->sfbCnt; ++idx) {
            int lo = band_lower_hz(enc_core(&e), idx);
            int hi = (int)(((int64_t)po->sfbOffsets[idx + 1] * 48000) / 2048);
            if (7500 >= lo && 7500 < hi) {
                BandObs o = observe_band(enc_core(&e), 0, idx);
                TASK_CHECK(!po->pnsFlag[idx]);
                TASK_CHECK(!o.all_noise_like);
                checked_band++;
            }
        }
    }
    TASK_CHECK(checked_band >= 5);
    enc_destroy(&e);
    return 0;
}

static int case_syntax_mono_adts_raw(void)
{
    Enc ea, er;
    int16_t pcm[1024];
    uint8_t buf[8192];
    size_t len;
    int saw_pns_frame = 0, normal_chain = 0;

    TASK_CHECK(enc_init(&ea, 48000, 1, 1, 1) == 0);
    for (int f = 0; f < 10; ++f) {
        ParsedFrame pf;
        AAC_ENCODER *core;
        Word32 logNorm, expectedGg;
        gen_frame(SIG_NOISE, 48000, 1, (unsigned)f, pcm);
        TASK_CHECK(enc_frame(&ea, pcm, buf, sizeof(buf), &len) == 0);
        TASK_CHECK(len > 7 && buf[0] == 0xff && (buf[1] & 0xf0) == 0xf0);
        {
            unsigned adts_len = ((unsigned)(buf[3] & 3) << 11) |
                                ((unsigned)buf[4] << 3) | (buf[5] >> 5);
            TASK_CHECK(adts_len == len);
        }
        core = enc_core(&ea);
        TASK_CHECK(parse_frame(buf, len, 1, core, &pf) == 0);
        TASK_CHECK(pf.end_bit == len * 8);
        TASK_CHECK(pf.ch[0].tns_present == 0);
        logNorm = -15 - (core->psyOut.psyOutChannel[0].mdctScale + 1);
        expectedGg = (Word32)core->qcOut.qcChannel[0].globalGain -
            core->qcOut.qcChannel[0].scf[core->qcOut.qcChannel[0].sectionData.firstScf] +
            100 - (logNorm << 2);
        TASK_CHECK((expectedGg & 255) == pf.ch[0].global_gain);
        if (pf.ch[0].pns_count > 0) {
            saw_pns_frame++;
            TASK_CHECK(pf.ch[0].pns_first_raw >= 0 && pf.ch[0].pns_first_raw < 512);
            TASK_CHECK(pf.ch[0].pns_count > 1);
        }
        for (int i = 0; i < pf.ch[0].total_sfb; ++i)
            if (pf.ch[0].cb[i] >= 1 && pf.ch[0].cb[i] <= 11) normal_chain = 1;
    }
    TASK_CHECK(saw_pns_frame >= 5);
    TASK_CHECK(normal_chain);
    enc_destroy(&ea);

    TASK_CHECK(enc_init(&er, 44100, 1, 0, 1) == 0);
    {
        int raw_pns_frames = 0;
        for (int f = 0; f < 6; ++f) {
            ParsedFrame pf;
            gen_frame(SIG_NOISE, 44100, 1, (unsigned)f + 40, pcm);
            TASK_CHECK(enc_frame(&er, pcm, buf, sizeof(buf), &len) == 0);
            TASK_CHECK(!(buf[0] == 0xff && (buf[1] & 0xf0) == 0xf0));
            TASK_CHECK(parse_frame(buf, len, 0, enc_core(&er), &pf) == 0);
            TASK_CHECK(pf.end_bit == len * 8);
            if (pf.ch[0].pns_count > 0) raw_pns_frames++;
        }
        TASK_CHECK(raw_pns_frames >= 4);
    }
    enc_destroy(&er);
    return 0;
}

static int case_syntax_stereo_ms(void)
{
    Enc e;
    int16_t pcm[2048];
    uint8_t buf[8192];
    size_t len;
    int saw_one_side = 0, saw_pns = 0;

    TASK_CHECK(enc_init(&e, 48000, 2, 1, 1) == 0);
    for (int f = 0; f < 12; ++f) {
        ParsedFrame pf;
        AAC_ENCODER *core;
        gen_frame(SIG_LEFT_NOISE, 48000, 2, (unsigned)f, pcm);
        TASK_CHECK(enc_frame(&e, pcm, buf, sizeof(buf), &len) == 0);
        core = enc_core(&e);
        TASK_CHECK(parse_frame(buf, len, 1, core, &pf) == 0);
        TASK_CHECK(pf.channels == 2 && pf.common_window == 1);
        for (int i = 0; i < pf.ch[0].total_sfb; ++i) {
            int p0 = core->psyOut.psyOutChannel[0].pnsFlag[i];
            int p1 = core->psyOut.psyOutChannel[1].pnsFlag[i];
            if (pf.ch[0].cb[i] == 13 || pf.ch[1].cb[i] == 13) {
                saw_pns++;
                TASK_CHECK(pf.ms_mask[i] == 0);
                TASK_CHECK(check_pns_energy(core, &pf,
                                           pf.ch[0].cb[i] == 13 ? 0 : 1, i));
                if ((pf.ch[0].cb[i] == 13) != (pf.ch[1].cb[i] == 13))
                    saw_one_side++;
            }
            TASK_CHECK(!((p0 || p1) && pf.ms_mask[i]));
        }
    }
    TASK_CHECK(saw_pns > 4);
    TASK_CHECK(saw_one_side > 0);
    enc_destroy(&e);
    return 0;
}

static int case_syntax_section_escape_parser(void)
{
    uint8_t bytes[2] = { 0xde, 0x40 }; /* cb13, escape 7, final length 1 */
    BR br;
    ParsedCh c;
    memset(&c, 0, sizeof(c));
    c.block_type = SHORT_WINDOW;
    c.num_groups = 1;
    c.max_sfb = 8;
    c.sfb_stride = 8;
    c.total_sfb = 8;
    br_init(&br, bytes, sizeof(bytes), 0);
    parse_sections(&br, &c);
    TASK_CHECK(!br.error);
    for (int i = 0; i < 8; ++i) TASK_CHECK(c.cb[i] == 13);
    return 0;
}

static int case_streaming_chunk_equivalence(void)
{
    Blobs whole = {0}, chunked = {0};
    TASK_CHECK(encode_whole_sequence(SIG_NOISE, 14, 1, &whole) == 0);
    TASK_CHECK(encode_chunked_sequence(SIG_NOISE, 14, &chunked) == 0);
    TASK_CHECK(whole.n == 14);
    TASK_CHECK(chunked.n == 14);
    TASK_CHECK(blobs_equal(&whole, &chunked));
    blobs_free(&whole);
    blobs_free(&chunked);
    return 0;
}

static int case_streaming_two_interleaved(void)
{
    enum { FRAMES = 10 };
    Enc a, b;
    int16_t *pa = malloc((size_t)FRAMES * 1024 * sizeof(int16_t));
    int16_t *pb = malloc((size_t)FRAMES * 1024 * sizeof(int16_t));
    Blobs refa = {0}, refb = {0}, outa = {0}, outb = {0};
    static const size_t chunks[] = { 2, 2046, 1024, 1024, 700, 1348, 4096, 8 };

    TASK_CHECK(pa && pb);
    for (int f = 0; f < FRAMES; ++f) {
        gen_frame(SIG_NOISE, 48000, 1, (unsigned)f, pa + f * 1024);
        gen_frame(SIG_TONE, 48000, 1, (unsigned)f + 100, pb + f * 1024);
    }
    TASK_CHECK(encode_whole_sequence(SIG_NOISE, FRAMES, 1, &refa) == 0);
    TASK_CHECK(encode_whole_sequence(SIG_TONE, FRAMES, 1, &refb) == 0);

    TASK_CHECK(enc_init(&a, 48000, 1, 1, 1) == 0);
    TASK_CHECK(enc_init(&b, 48000, 1, 1, 1) == 0);
    {
        size_t offa = 0, offb = 0, ci = 0;
        size_t total = (size_t)FRAMES * 2048;
        while (offa < total || offb < total) {
            size_t na = chunks[ci % 8], nb = chunks[(ci + 3) % 8];
            VO_CODECBUFFER in;
            if (na > total - offa) na = total - offa;
            memset(&in, 0, sizeof(in));
            in.Buffer = (VO_PBYTE)((uint8_t *)pa + offa); in.Length = (VO_U32)na;
            TASK_CHECK(a.api.SetInputData(a.h, &in) == 0);
            TASK_CHECK(drain_outputs(&a, &outa) == 0);
            offa += na;

            if (nb > total - offb) nb = total - offb;
            memset(&in, 0, sizeof(in));
            in.Buffer = (VO_PBYTE)((uint8_t *)pb + offb); in.Length = (VO_U32)nb;
            TASK_CHECK(b.api.SetInputData(b.h, &in) == 0);
            TASK_CHECK(drain_outputs(&b, &outb) == 0);
            offb += nb;
            ci++;
        }
    }
    TASK_CHECK(outa.n == FRAMES && outb.n == FRAMES);
    TASK_CHECK(blobs_equal(&refa, &outa));
    TASK_CHECK(blobs_equal(&refb, &outb));
    enc_destroy(&a);
    enc_destroy(&b);
    blobs_free(&refa); blobs_free(&refb); blobs_free(&outa); blobs_free(&outb);
    free(pa); free(pb);
    return 0;
}

static int case_streaming_default_off_bytes(void)
{
    VO_AUDIO_CODECAPI api = {0};
    VO_HANDLE h = 0;
    VO_MEM_OPERATOR mem = {0};
    VO_CODEC_INIT_USERDATA u = {0};
    AACENC_PARAM p = {48000, 96000, 2, 1};
    int16_t pcm[2048];
    uint8_t bytes[8192];
    uint32_t rng = 91, hash = 2166136261u;
    unsigned frames = 0, total = 0;

    mem.Alloc = cmnMemAlloc; mem.Copy = cmnMemCopy; mem.Free = cmnMemFree;
    mem.Set = cmnMemSet; mem.Check = cmnMemCheck;
    u.memflag = VO_IMF_USERMEMOPERATOR; u.memData = &mem;
    TASK_CHECK(voGetAACEncAPI(&api) == 0);
    TASK_CHECK(api.Init(&h, VO_AUDIO_CodingAAC, &u) == 0);
    TASK_CHECK(api.SetParam(h, VO_PID_AAC_ENCPARAM, &p) == 0);
    TASK_CHECK(enc_get_pns(&(Enc){ .api = api, .h = h }) == 0);

    for (unsigned k = 0; k < 24; ++k) {
        VO_CODECBUFFER in = {0}, out = {0};
        VO_AUDIO_OUTPUTINFO info = {0};
        unsigned len;
        for (unsigned i = 0; i < 2048; ++i) {
            rng = rng * 1664525u + 1013904223u;
            pcm[i] = (int16_t)((int32_t)(rng >> 17) - 16384);
            if (k % 6 < 3) pcm[i] /= 32;
        }
        in.Buffer = (VO_PBYTE)pcm; in.Length = sizeof(pcm);
        out.Buffer = bytes; out.Length = sizeof(bytes);
        TASK_CHECK(api.SetInputData(h, &in) == 0);
        TASK_CHECK(api.GetOutputData(h, &out, &info) == 0);
        len = ((unsigned)(bytes[3] & 3) << 11) |
              ((unsigned)bytes[4] << 3) | (bytes[5] >> 5);
        TASK_CHECK(out.Length == len && len > 7);
        for (unsigned j = 7; j < len; ++j)
            hash = (hash ^ bytes[j]) * 16777619u;
        frames++;
        total += len;
    }
    TASK_CHECK(frames == 24);
    TASK_CHECK(total == 6520);
    TASK_CHECK(hash == 0xe30e5636u);
    api.Uninit(h);
    return 0;
}

const struct task_case task_cases[] = {
    {"selection", "criteria_and_energy", case_selection_noise_bands},
    {"selection", "narrow_band_tone_rejected", case_selection_tone_rejected},
    {"syntax", "param_validation_and_timing", case_param_validation_timing},
    {"syntax", "mono_adts_and_raw", case_syntax_mono_adts_raw},
    {"syntax", "stereo_ms_and_one_sided_pns", case_syntax_stereo_ms},
    {"syntax", "section_escape_parser", case_syntax_section_escape_parser},
    {"streaming", "whole_vs_irregular_even_chunks", case_streaming_chunk_equivalence},
    {"streaming", "two_interleaved_instances", case_streaming_two_interleaved},
    {"streaming", "default_off_byte_compatibility", case_streaming_default_off_bytes},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
