/* Regression for the explicit frame-boundary PCE update interface
   (NeAACDecUpdatePCE).

   A continuous AAC-LC raw stream with ordinary non-zero spectra runs
   through long, long-start, eight-short and long-stop windows while the
   active layout is changed between complete access units:
     L1 = front SCE0 + CPE0   (frames 0..3)
     L2 = front CPE0 + SCE9   (frames 4..7,  CPE0 kept, SCE0 retired)
     L3 = front SCE9          (frames 8..9,  CPE0 retired)
     L4 = front SCE9 + SCE0 + CPE0 (frames 10..15, SCE0/CPE0 re-join fresh)
   Surviving identities are compared against continuously decoded
   references, new identities against independent cold-start references,
   and the encounter order of the elements inside the access units is
   permuted once identity based decoding is active. */

#include "common.h"
#include "structs.h"
#include "check.h"
#include "aac_fixture.h"
#include "layout_asc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NFRAMES 16
#define PCM_SCALE (1.0/32768.0)

/* window sequences of the continuous stream: legal transitions covering
   ONLY_LONG, LONG_START, EIGHT_SHORT and LONG_STOP */
static const unsigned frame_seq[NFRAMES] =
    {0,1,2,2,3,0,1,2,2,3,0,1,2,2,3,0};

static const struct fixture_element L1[] = {{0,0},{1,0}};         /* SCE0 CPE0 */
static const struct fixture_element L1R[] = {{1,0},{0,0}};
static const struct fixture_element L2[] = {{1,0},{0,9}};         /* CPE0 SCE9 */
static const struct fixture_element L2R[] = {{0,9},{1,0}};
static const struct fixture_element L3[] = {{0,9}};               /* SCE9 */
static const struct fixture_element L4[] = {{0,9},{0,0},{1,0}};   /* SCE9 SCE0 CPE0 */
static const struct fixture_element L4A[] = {{1,0},{0,0},{0,9}};
static const struct fixture_element L4B[] = {{0,0},{1,0},{0,9}};
static const struct fixture_element L4C[] = {{0,9},{1,0},{0,0}};
static const struct fixture_element L4D[] = {{1,0},{0,9},{0,0}};

/* encounter order of the DUT access units; permuted once identity based
   decoding is active (after the first update at frame 2) */
static const struct fixture_element *dut_order(unsigned i, size_t *n)
{
    switch (i) {
    case 0: case 1: case 2: *n = 2; return L1;
    case 3:  *n = 2; return L1R;
    case 4: case 7: *n = 2; return L2;
    case 5: case 6: *n = 2; return L2R;
    case 8: case 9: *n = 1; return L3;
    case 11: *n = 3; return L4A;
    case 12: *n = 3; return L4B;
    case 13: *n = 3; return L4C;
    case 14: *n = 3; return L4D;
    default: *n = 3; return L4;
    }
}

static unsigned era_channels(unsigned i) { return i < 8 ? 3 : (i < 10 ? 1 : 4); }

static void check_close(const real_t *got, const real_t *ref, size_t n)
{
    size_t k;
    TASK_CHECK(got != NULL && ref != NULL);
    for (k = 0; k < n; k++) {
        double d = fabs((double)got[k] - (double)ref[k]);
        TASK_CHECK(d <= 1e-6 * (1.0 + fabs((double)ref[k])));
    }
}

static void check_pcm_ch(const float *dpcm, unsigned dtot, unsigned dch,
                         const float *rpcm, unsigned rtot, unsigned rch)
{
    unsigned k;
    for (k = 0; k < 1024; k++) {
        double g = dpcm[k*dtot + dch];
        double r = rpcm[k*rtot + rch];
        TASK_CHECK(fabs(g - r) <= 1e-6 * (1.0 + fabs(r)));
    }
}

static void check_pcm_ch_vs_time_out(const float *dpcm, unsigned dtot, unsigned dch,
                                     const real_t *rto)
{
    unsigned k;
    for (k = 0; k < 1024; k++) {
        double g = dpcm[k*dtot + dch];
        double r = (double)rto[k] * PCM_SCALE;
        TASK_CHECK(fabs(g - r) <= 1e-6 * (1.0 + fabs(r)));
    }
}

/* compare one channel of one identity between the active instance and a
   reference decoder; cold=1 means the reference just cold-started this
   identity (its first frame is suppressed, samples==0) so the public PCM
   of the active instance is checked against the reference's read-only
   synthesis output instead */
static void compare_channel(NeAACDecStruct *dst, const float *dpcm, unsigned dtot,
                            unsigned dslot, NeAACDecStruct *rst, const float *rpcm,
                            unsigned rtot, unsigned rslot, int cold)
{
    TASK_CHECK(dst->window_shape_prev[dslot] == rst->window_shape_prev[rslot]);
    check_close(dst->fb_intermed[dslot], rst->fb_intermed[rslot], 1024);
    check_close(dst->time_out[dslot], rst->time_out[rslot], 1024);
    if (cold == 1)
        check_pcm_ch_vs_time_out(dpcm, dtot, dslot, rst->time_out[rslot]);
    else if (cold == 0)
        check_pcm_ch(dpcm, dtot, dslot, rpcm, rtot, rslot);
}

static NeAACDecHandle open_dec(const struct fixture_element *es, size_t n,
                               unsigned char expect_ch)
{
    unsigned char asc[64];
    size_t m;
    unsigned long rate = 0;
    unsigned char ch = 0;
    NeAACDecHandle h;
    NeAACDecConfigurationPtr c;
    m = fixture_asc(asc, sizeof asc, es, n);
    TASK_CHECK(m != 0);
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    c = NeAACDecGetCurrentConfiguration(h);
    c->outputFormat = FAAD_FMT_FLOAT;
    c->downMatrix = 0;
    c->dontUpSampleImplicitSBR = 1;
    TASK_CHECK(NeAACDecSetConfiguration(h, c));
    TASK_CHECK(NeAACDecInit2(h, asc, (unsigned long)m, &rate, &ch) == 0);
    TASK_CHECK(rate == 48000 && ch == expect_ch);
    return h;
}

static void *decode_frame(NeAACDecHandle h, const struct fixture_element *es,
                          size_t n, unsigned i, unsigned gain_base,
                          NeAACDecFrameInfo *info)
{
    unsigned char packet[256];
    size_t m;
    m = fixture_packet(packet, sizeof packet, es, n,
                       frame_seq[i], i%2, gain_base + i);
    TASK_CHECK(m != 0);
    return NeAACDecDecode(h, info, packet, (unsigned long)m);
}

static void do_update(NeAACDecHandle h, const struct fixture_element *es,
                      size_t n, int expect)
{
    unsigned char asc[64];
    size_t m;
    m = fixture_asc(asc, sizeof asc, es, n);
    TASK_CHECK(m != 0);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == expect);
    /* a successful update must not retain the caller's buffer */
    memset(asc, 0xA5, sizeof asc);
}

/* decoder state snapshot used to prove that rejected updates change nothing */
struct snap {
    real_t time_out[8][1024];
    real_t fb[8][1024];
    const real_t *to_ptr[8];
    const real_t *fb_ptr[8];
    uint8_t shape[8];
    uint32_t frame;
    uint8_t channels;
};

static void take_snap(NeAACDecStruct *s, struct snap *sn)
{
    unsigned k;
    sn->frame = s->frame;
    sn->channels = s->pce.channels;
    for (k = 0; k < 8; k++) {
        sn->shape[k] = s->window_shape_prev[k];
        sn->to_ptr[k] = s->time_out[k];
        sn->fb_ptr[k] = s->fb_intermed[k];
        if (s->time_out[k]) memcpy(sn->time_out[k], s->time_out[k], sizeof sn->time_out[k]);
        else memset(sn->time_out[k], 0, sizeof sn->time_out[k]);
        if (s->fb_intermed[k]) memcpy(sn->fb[k], s->fb_intermed[k], sizeof sn->fb[k]);
        else memset(sn->fb[k], 0, sizeof sn->fb[k]);
    }
}

static void check_snap(NeAACDecStruct *s, const struct snap *sn)
{
    unsigned k;
    TASK_CHECK(s->frame == sn->frame);
    TASK_CHECK(s->pce.channels == sn->channels);
    for (k = 0; k < 8; k++) {
        TASK_CHECK(s->window_shape_prev[k] == sn->shape[k]);
        TASK_CHECK(s->time_out[k] == sn->to_ptr[k]);
        TASK_CHECK(s->fb_intermed[k] == sn->fb_ptr[k]);
        if (sn->to_ptr[k]) TASK_CHECK(memcmp(s->time_out[k], sn->time_out[k], sizeof sn->time_out[k]) == 0);
        if (sn->fb_ptr[k]) TASK_CHECK(memcmp(s->fb_intermed[k], sn->fb[k], sizeof sn->fb[k]) == 0);
    }
}

/* every one of these must be rejected with -1 and must leave the
   decoder state untouched (verified by the caller via struct snap) */
static void invalid_updates(NeAACDecHandle h)
{
    unsigned char asc[80];
    unsigned char valid[64];
    struct layout_asc_spec sp;
    size_t m, vm;
    NeAACDecHandle scratch;
    unsigned long rate;
    unsigned char ch;

    vm = fixture_asc(valid, sizeof valid, L1, 2);
    TASK_CHECK(vm != 0);

    /* LFE element */
    layout_asc_defaults(&sp); sp.n_lfe = 1; sp.lfe[0] = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* more than 8 channels: 4 CPE + 1 SCE = 9 */
    layout_asc_defaults(&sp);
    sp.front[0].pair=1; sp.front[0].tag=0;
    sp.front[1].pair=1; sp.front[1].tag=1;
    sp.front[2].pair=1; sp.front[2].tag=2;
    sp.front[3].pair=1; sp.front[3].tag=3;
    sp.front[4].pair=0; sp.front[4].tag=4;
    sp.n_front = 5;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* duplicated SCE tag */
    layout_asc_defaults(&sp);
    sp.front[0].pair=0; sp.front[0].tag=0;
    sp.front[1].pair=0; sp.front[1].tag=0;
    sp.n_front = 2;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* duplicated CPE tag across element groups */
    layout_asc_defaults(&sp);
    sp.front[0].pair=1; sp.front[0].tag=0; sp.n_front=1;
    sp.back[0].pair=1; sp.back[0].tag=0; sp.n_back=1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* no front element */
    layout_asc_defaults(&sp);
    sp.n_front = 0; sp.side[0].pair=0; sp.side[0].tag=1; sp.n_side=1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* mixdown signalling */
    layout_asc_defaults(&sp); sp.mono_mix = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);
    layout_asc_defaults(&sp); sp.stereo_mix = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);
    layout_asc_defaults(&sp); sp.matrix_mix = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* associated data and coupling elements */
    layout_asc_defaults(&sp); sp.n_assoc = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);
    layout_asc_defaults(&sp); sp.n_cc = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* PCE sampling rate / profile disagreeing with the ASC */
    layout_asc_defaults(&sp); sp.pce_sf_index = 4;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);
    layout_asc_defaults(&sp); sp.pce_profile = 0;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* ASC object type different from the active AAC-LC configuration */
    layout_asc_defaults(&sp); sp.profile = 1; sp.pce_profile = 0;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* channelConfiguration != 0 (no explicit PCE) */
    layout_asc_defaults(&sp); sp.chan_cfg = 2;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* 960 sample frames */
    layout_asc_defaults(&sp); sp.frame_len_flag = 1;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)m) == -1);

    /* incomplete configuration: truncated valid ASC */
    layout_asc_defaults(&sp);
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m > 4);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, 4) == -1);

    /* degenerate inputs */
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, 1) == -1);
    TASK_CHECK(NeAACDecUpdatePCE(h, NULL, 16) == -1);
    TASK_CHECK(NeAACDecUpdatePCE(NULL, asc, (unsigned long)m) == -1);

    /* a handle that was never initialized */
    scratch = NeAACDecOpen();
    TASK_CHECK(scratch != NULL);
    TASK_CHECK(NeAACDecUpdatePCE(scratch, valid, (unsigned long)vm) == -1);
    NeAACDecClose(scratch);

    /* a handle initialized without an explicit PCE */
    layout_asc_defaults(&sp); sp.chan_cfg = 2;
    m = layout_asc_write(asc, sizeof asc, &sp); TASK_CHECK(m);
    scratch = NeAACDecOpen();
    TASK_CHECK(scratch != NULL);
    rate = 0; ch = 0;
    TASK_CHECK(NeAACDecInit2(scratch, asc, (unsigned long)m, &rate, &ch) == 0);
    TASK_CHECK(ch == 2);
    TASK_CHECK(NeAACDecUpdatePCE(scratch, valid, (unsigned long)vm) == -1);
    NeAACDecClose(scratch);
}

struct session {
    NeAACDecHandle dut;  /* device under test */
    NeAACDecHandle rcpe; /* L1 layout, continuous CPE0 reference, frames 0..7 */
    NeAACDecHandle rs9;  /* L2 layout, cold-started SCE9 reference, frames 4..15 */
    NeAACDecHandle rl4;  /* L4 layout, cold-started SCE0/CPE0 reference, frames 10..15 */
    unsigned gain_base;
};

static void session_open(struct session *s, unsigned gain_base)
{
    s->gain_base = gain_base;
    s->dut  = open_dec(L1, 2, 3);
    s->rcpe = open_dec(L1, 2, 3);
    s->rs9  = open_dec(L2, 2, 3);
    s->rl4  = open_dec(L4, 3, 4);
}

static void session_close(struct session *s)
{
    NeAACDecClose(s->dut);
    NeAACDecClose(s->rcpe);
    NeAACDecClose(s->rs9);
    NeAACDecClose(s->rl4);
}

/* layout updates scheduled before the given frame is decoded */
static void scheduled_updates(struct session *s, unsigned i)
{
    NeAACDecStruct *st = (NeAACDecStruct*)s->dut;
    const real_t *fb0, *fb1, *fb2, *fb3, *to0, *to1, *to2, *to3;
    uint8_t sh0, sh1, sh2, sh3;
    struct snap sn;

    switch (i) {
    case 2:
        /* rejected updates must not disturb the active L1 layout */
        take_snap(st, &sn);
        invalid_updates(s->dut);
        check_snap(st, &sn);
        /* same-value update L1 -> L1: accepted, history preserved */
        fb0 = st->fb_intermed[0]; fb1 = st->fb_intermed[1]; fb2 = st->fb_intermed[2];
        sh0 = st->window_shape_prev[0]; sh1 = st->window_shape_prev[1];
        sh2 = st->window_shape_prev[2];
        do_update(s->dut, L1, 2, 0);
        TASK_CHECK(st->fb_intermed[0] == fb0 && st->fb_intermed[1] == fb1 &&
                   st->fb_intermed[2] == fb2);
        TASK_CHECK(st->window_shape_prev[0] == sh0 && st->window_shape_prev[1] == sh1 &&
                   st->window_shape_prev[2] == sh2);
        TASK_CHECK(st->pce.channels == 3 && st->pce_update == 1);
        break;
    case 4:
        /* L1 -> L2: CPE0 moves from slots 1,2 to slots 0,1 and keeps its
           history, SCE0 is retired, SCE9 starts from zero history */
        fb1 = st->fb_intermed[1]; fb2 = st->fb_intermed[2];
        to1 = st->time_out[1]; to2 = st->time_out[2];
        sh1 = st->window_shape_prev[1]; sh2 = st->window_shape_prev[2];
        TASK_CHECK(fb1 != NULL && fb2 != NULL);
        do_update(s->dut, L2, 2, 0);
        TASK_CHECK(st->fb_intermed[0] == fb1 && st->fb_intermed[1] == fb2 &&
                   st->fb_intermed[2] == NULL);
        TASK_CHECK(st->time_out[0] == to1 && st->time_out[1] == to2 &&
                   st->time_out[2] == NULL);
        TASK_CHECK(st->window_shape_prev[0] == sh1 && st->window_shape_prev[1] == sh2 &&
                   st->window_shape_prev[2] == 0);
        TASK_CHECK(st->pce.channels == 3);
        break;
    case 8:
        /* rejected updates must not disturb the active L2 layout either */
        take_snap(st, &sn);
        invalid_updates(s->dut);
        check_snap(st, &sn);
        /* L2 -> L3: SCE9 moves from slot 2 to slot 0, CPE0 is retired */
        fb2 = st->fb_intermed[2];
        to2 = st->time_out[2];
        sh2 = st->window_shape_prev[2];
        TASK_CHECK(fb2 != NULL);
        do_update(s->dut, L3, 1, 0);
        TASK_CHECK(st->fb_intermed[0] == fb2 && st->fb_intermed[1] == NULL &&
                   st->fb_intermed[2] == NULL);
        TASK_CHECK(st->time_out[0] == to2 && st->time_out[1] == NULL &&
                   st->time_out[2] == NULL);
        TASK_CHECK(st->window_shape_prev[0] == sh2 && st->window_shape_prev[1] == 0);
        TASK_CHECK(st->pce.channels == 1);
        break;
    case 10:
        /* L3 -> L4: SCE9 stays in slot 0, SCE0 and CPE0 re-join as new
           identities with zero history */
        fb0 = st->fb_intermed[0];
        to0 = st->time_out[0];
        sh0 = st->window_shape_prev[0];
        TASK_CHECK(fb0 != NULL);
        do_update(s->dut, L4, 3, 0);
        TASK_CHECK(st->fb_intermed[0] == fb0);
        TASK_CHECK(st->time_out[0] == to0);
        TASK_CHECK(st->window_shape_prev[0] == sh0);
        TASK_CHECK(st->fb_intermed[1] == NULL && st->fb_intermed[2] == NULL &&
                   st->fb_intermed[3] == NULL);
        TASK_CHECK(st->time_out[1] == NULL && st->time_out[2] == NULL &&
                   st->time_out[3] == NULL);
        TASK_CHECK(st->pce.channels == 4);
        break;
    case 14:
        /* same-value update L4 -> L4: nothing is reallocated or reset */
        fb0 = st->fb_intermed[0]; fb1 = st->fb_intermed[1];
        fb2 = st->fb_intermed[2]; fb3 = st->fb_intermed[3];
        to0 = st->time_out[0]; to1 = st->time_out[1];
        to2 = st->time_out[2]; to3 = st->time_out[3];
        sh0 = st->window_shape_prev[0]; sh1 = st->window_shape_prev[1];
        sh2 = st->window_shape_prev[2]; sh3 = st->window_shape_prev[3];
        TASK_CHECK(fb0 != NULL && fb1 != NULL && fb2 != NULL && fb3 != NULL);
        do_update(s->dut, L4, 3, 0);
        TASK_CHECK(st->fb_intermed[0] == fb0 && st->fb_intermed[1] == fb1 &&
                   st->fb_intermed[2] == fb2 && st->fb_intermed[3] == fb3);
        TASK_CHECK(st->time_out[0] == to0 && st->time_out[1] == to1 &&
                   st->time_out[2] == to2 && st->time_out[3] == to3);
        TASK_CHECK(st->window_shape_prev[0] == sh0 && st->window_shape_prev[1] == sh1 &&
                   st->window_shape_prev[2] == sh2 && st->window_shape_prev[3] == sh3);
        break;
    default:
        break;
    }
}

static void check_positions(const NeAACDecFrameInfo *info, unsigned i)
{
    TASK_CHECK(info->num_side_channels == 0);
    TASK_CHECK(info->num_back_channels == 0);
    TASK_CHECK(info->num_lfe_channels == 0);
    if (i < 8) {
        TASK_CHECK(info->num_front_channels == 3);
        TASK_CHECK(info->channel_position[0] == FRONT_CHANNEL_CENTER);
        TASK_CHECK(info->channel_position[1] == FRONT_CHANNEL_LEFT);
        TASK_CHECK(info->channel_position[2] == FRONT_CHANNEL_RIGHT);
    } else if (i < 10) {
        TASK_CHECK(info->num_front_channels == 1);
        TASK_CHECK(info->channel_position[0] == FRONT_CHANNEL_CENTER);
    } else {
        TASK_CHECK(info->num_front_channels == 4);
        TASK_CHECK(info->channel_position[0] == FRONT_CHANNEL_LEFT);
        TASK_CHECK(info->channel_position[1] == FRONT_CHANNEL_RIGHT);
        TASK_CHECK(info->channel_position[2] == FRONT_CHANNEL_LEFT);
        TASK_CHECK(info->channel_position[3] == FRONT_CHANNEL_RIGHT);
    }
}

static void session_step(struct session *s, unsigned i)
{
    NeAACDecStruct *dst = (NeAACDecStruct*)s->dut;
    NeAACDecFrameInfo info, rinfo;
    const struct fixture_element *order;
    size_t n;
    float *pcm;
    unsigned ch = era_channels(i);

    scheduled_updates(s, i);

    order = dut_order(i, &n);
    pcm = (float*)decode_frame(s->dut, order, n, i, s->gain_base, &info);
    TASK_CHECK(pcm != NULL && info.error == 0);
    TASK_CHECK(info.channels == ch);
    TASK_CHECK(info.samplerate == 48000);
    TASK_CHECK(info.object_type == 2); /* AAC LC */
    /* the global decode progress never restarts: only the very first
       frame of the whole stream is suppressed */
    TASK_CHECK(info.samples == (i == 0 ? 0u : 1024u*ch));
    check_positions(&info, i);
    TASK_CHECK(dst->frame == i+1);

    if (i < 8) {
        /* continuous CPE0 reference (and SCE0 while L1 is active) */
        NeAACDecStruct *rst = (NeAACDecStruct*)s->rcpe;
        float *rpcm = (float*)decode_frame(s->rcpe, L1, 2, i, s->gain_base, &rinfo);
        TASK_CHECK(rpcm != NULL && rinfo.error == 0);
        TASK_CHECK(rinfo.samples == (i == 0 ? 0u : 3072u));
        if (i < 4) {
            /* L1 era: SCE0 slot 0, CPE0 slots 1,2 on both instances */
            compare_channel(dst, pcm, ch, 0, rst, rpcm, 3, 0, i == 0 ? 2 : 0);
            compare_channel(dst, pcm, ch, 1, rst, rpcm, 3, 1, i == 0 ? 2 : 0);
            compare_channel(dst, pcm, ch, 2, rst, rpcm, 3, 2, i == 0 ? 2 : 0);
        } else {
            /* L2 era: CPE0 kept its history across the update and moved
               to slots 0,1 (PCM channels 0,1) while the reference stays
               in slots 1,2 (PCM channels 1,2) */
            compare_channel(dst, pcm, ch, 0, rst, rpcm, 3, 1, 0);
            compare_channel(dst, pcm, ch, 1, rst, rpcm, 3, 2, 0);
        }
    }

    if (i >= 4) {
        /* SCE9: new identity at frame 4, kept through L2 -> L3 -> L4;
           the reference cold-starts it in the L2 layout */
        NeAACDecStruct *rst = (NeAACDecStruct*)s->rs9;
        float *rpcm = (float*)decode_frame(s->rs9, L2, 2, i, s->gain_base, &rinfo);
        unsigned dslot = (i < 8) ? 2 : 0;
        TASK_CHECK(rpcm != NULL && rinfo.error == 0);
        TASK_CHECK(rinfo.samples == (i == 4 ? 0u : 3072u));
        compare_channel(dst, pcm, ch, dslot, rst, rpcm, 3, 2, i == 4 ? 1 : 0);
    }

    if (i >= 10) {
        /* SCE0 and CPE0 retired earlier and re-joined at frame 10: they
           must behave like the cold-started identities of the L4
           reference, the retired tails must not come back */
        NeAACDecStruct *rst = (NeAACDecStruct*)s->rl4;
        float *rpcm = (float*)decode_frame(s->rl4, L4, 3, i, s->gain_base, &rinfo);
        int cold = (i == 10) ? 1 : 0;
        TASK_CHECK(rpcm != NULL && rinfo.error == 0);
        TASK_CHECK(rinfo.samples == (i == 10 ? 0u : 4096u));
        compare_channel(dst, pcm, ch, 1, rst, rpcm, 4, 1, cold); /* SCE0 */
        compare_channel(dst, pcm, ch, 2, rst, rpcm, 4, 2, cold); /* CPE0 left */
        compare_channel(dst, pcm, ch, 3, rst, rpcm, 4, 3, cold); /* CPE0 right */
    }
}

static unsigned decode_error(struct session *s, const struct fixture_element *es,
                             size_t n)
{
    unsigned char packet[256];
    NeAACDecFrameInfo info;
    size_t m;
    m = fixture_packet(packet, sizeof packet, es, n, 0, 0, s->gain_base);
    TASK_CHECK(m != 0);
    NeAACDecDecode(s->dut, &info, packet, (unsigned long)m);
    return info.error;
}

/* identity based decoding must still reject malformed access units */
static void session_tail(struct session *s)
{
    static const struct fixture_element missing[] = {{0,9},{1,0}}; /* SCE0 absent */
    static const struct fixture_element dup[] = {{0,9},{0,0},{1,0},{0,9}};
    static const struct fixture_element unknown[] = {{0,5}};
    TASK_CHECK(decode_error(s, missing, 2) == 21);
    TASK_CHECK(decode_error(s, dup, 4) == 21);
    TASK_CHECK(decode_error(s, unknown, 1) == 21);
}

void task_cases(void)
{
    struct session a, b;
    unsigned i;

    /* two independent instances with different spectra, interleaved
       frame by frame to prove the layout updates are per-handle */
    session_open(&a, 100);
    session_open(&b, 150);
    for (i = 0; i < NFRAMES; i++) {
        session_step(&a, i);
        session_step(&b, i);
    }
    session_tail(&a);
    session_close(&a);
    session_close(&b);
}
