/* Regression: frame-boundary PCE layout update (NeAACDecUpdatePCE).

   Continuous AAC-LC raw streams switch their channel layout between
   access units. Identities that survive an update must keep their
   synthesis history (overlap + previous window shape), new identities
   must behave like an independent cold start, and retired identities
   must not be revived. Element order inside an access unit may differ
   from the PCE order and may change per frame. */

#include "check.h"
#include "common.h"
#include "structs.h"
#include "aac_fixture.h"
#include "layout_ref.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* comparison helpers (tolerance 1e-6*(1+abs(reference)))             */

static int close1(double got, double ref) {
    return fabs(got-ref) <= 1e-6*(1.0+fabs(ref));
}
static int chan_close(const float *a, unsigned as,
                      const float *b, unsigned bs, unsigned long n) {
    unsigned long i;
    for(i=0;i<n;++i) if(!close1(a[i*as], b[i*bs])) return 0;
    return 1;
}
static int reals_close(const real_t *a, const real_t *b, unsigned long n) {
    unsigned long i;
    for(i=0;i<n;++i) if(!close1(a[i], b[i])) return 0;
    return 1;
}
static int pcm_matches_timeout(const float *pcm, unsigned nch, unsigned slot,
                               const real_t *to) {
    unsigned i;
    for(i=0;i<1024;++i) {
        double expect = (double)to[i]*(1.0/32768.0);
        if(!close1(pcm[i*nch+slot], expect)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* decoder instances and single-element reference decoders            */

struct inst {
    NeAACDecHandle h;
    NeAACDecStruct *st;
    NeAACDecFrameInfo info;
    const float *pcm;
    unsigned long samples;
    unsigned nch;
    unsigned frame; /* our own count of successfully decoded frames */
};

struct ref {
    NeAACDecHandle h;
    NeAACDecStruct *st;
    NeAACDecFrameInfo info;
    unsigned pair, tag, nch, n;
    float pcm[2048];
    unsigned long samples;
};

static void inst_open(struct inst *a, const unsigned char *asc, unsigned long n) {
    a->h = layout_open(asc, n);
    TASK_CHECK(a->h != NULL);
    a->st = (NeAACDecStruct*)a->h;
    a->frame = 0;
    a->pcm = NULL; a->samples = 0; a->nch = 0;
}

static void inst_decode(struct inst *a, const struct fixture_element *order,
                        unsigned n, unsigned seq, unsigned shape, unsigned gain) {
    unsigned char pkt[256];
    size_t z = fixture_packet(pkt, sizeof pkt, order, n, seq, shape, gain);
    TASK_CHECK(z > 0);
    a->pcm = NeAACDecDecode(a->h, &a->info, pkt, (unsigned long)z);
    TASK_CHECK(a->pcm != NULL);
    TASK_CHECK(a->info.error == 0);
    a->samples = a->info.samples;
    a->nch = a->info.channels;
    ++a->frame;
    /* global decode progress must never restart */
    TASK_CHECK(a->st->frame == a->frame);
}

static void inst_decode_error(struct inst *a, const struct fixture_element *order,
                              unsigned n, unsigned seq, unsigned shape, unsigned gain) {
    unsigned char pkt[256];
    size_t z = fixture_packet(pkt, sizeof pkt, order, n, seq, shape, gain);
    void *p;
    TASK_CHECK(z > 0);
    p = NeAACDecDecode(a->h, &a->info, pkt, (unsigned long)z);
    TASK_CHECK(p == NULL);
    TASK_CHECK(a->info.error != 0);
}

static void ref_open1(struct ref *r, unsigned pair, unsigned tag) {
    struct fixture_element e = {pair, tag};
    unsigned char asc[64];
    size_t n = fixture_asc(asc, sizeof asc, &e, 1);
    TASK_CHECK(n > 0);
    r->h = layout_open(asc, (unsigned long)n);
    TASK_CHECK(r->h != NULL);
    r->st = (NeAACDecStruct*)r->h;
    r->pair = pair; r->tag = tag;
    r->nch = pair?2:1;
    r->n = 0; r->samples = 0;
}

static void ref_step(struct ref *r, unsigned seq, unsigned shape, unsigned gain) {
    struct fixture_element e = {r->pair, r->tag};
    unsigned char pkt[256];
    size_t z = fixture_packet(pkt, sizeof pkt, &e, 1, seq, shape, gain);
    void *pcm;
    TASK_CHECK(z > 0);
    pcm = NeAACDecDecode(r->h, &r->info, pkt, (unsigned long)z);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(r->info.error == 0);
    r->samples = r->info.samples;
    TASK_CHECK(r->samples <= sizeof r->pcm/sizeof r->pcm[0]);
    if(r->samples) memcpy(r->pcm, pcm, r->samples*sizeof(float));
    ++r->n;
}

static void ref_close(struct ref *r) { NeAACDecClose(r->h); r->h = NULL; }

/* Compare one channel of the active instance with one channel of a
   single-element reference. On the reference's first (cold) frame its
   public output is suppressed, so compare the read-only synthesis state
   (time_out, fb_intermed, previous window shape) instead; afterwards
   compare the public PCM. */
static void check_chan(struct inst *a, unsigned aslot,
                       struct ref *r, unsigned rch) {
    if(r->n == 1) {
        TASK_CHECK(r->samples == 0);
        TASK_CHECK(reals_close(a->st->time_out[aslot], r->st->time_out[rch], 1024));
        TASK_CHECK(reals_close(a->st->fb_intermed[aslot], r->st->fb_intermed[rch], 1024));
        TASK_CHECK(a->st->window_shape_prev[aslot] == r->st->window_shape_prev[rch]);
        if(a->st->frame > 1) {
            /* the active instance still outputs the complete PCM */
            TASK_CHECK(a->samples == 1024ul*a->nch);
            TASK_CHECK(pcm_matches_timeout(a->pcm, a->nch, aslot,
                                           r->st->time_out[rch]));
        }
    } else {
        TASK_CHECK(r->samples == 1024ul*r->nch);
        TASK_CHECK(a->samples == 1024ul*a->nch);
        TASK_CHECK(chan_close(a->pcm+aslot, a->nch, r->pcm+rch, r->nch, 1024));
        TASK_CHECK(reals_close(a->st->time_out[aslot], r->st->time_out[rch], 1024));
        TASK_CHECK(reals_close(a->st->fb_intermed[aslot], r->st->fb_intermed[rch], 1024));
        TASK_CHECK(a->st->window_shape_prev[aslot] == r->st->window_shape_prev[rch]);
    }
}

static void check_ele(struct inst *a, unsigned aslot, struct ref *r) {
    check_chan(a, aslot, r, 0);
    if(r->nch == 2) check_chan(a, aslot+1, r, 1);
}

/* ------------------------------------------------------------------ */

static const unsigned SEQ5[5] = {0,1,2,2,3}; /* long, start, 8xshort, stop */
#define FSEQ(f)   SEQ5[(f)%5]
#define FSHAPE(f) ((f)&1u)
#define GAIN_A(f) (100u + (((f)*7u)%8u)*11u)
#define GAIN_B(f) (105u + (((f)*5u)%8u)*11u)

static void spec_front(struct asc_spec *s, const struct fixture_element *e,
                       unsigned n) {
    asc_spec_init(s);
    s->nfront = n;
    memcpy(s->front, e, n*sizeof *e);
}

/* main instance: layout updates, kept/new/retired identities */
static const struct fixture_element A_L0[2]  = {{0,0},{1,0}};        /* SCE0 CPE0 */
static const struct fixture_element A_L1a[2] = {{1,0},{0,9}};        /* CPE0 SCE9 */
static const struct fixture_element A_L1b[2] = {{0,9},{1,0}};        /* SCE9 CPE0 */
static const struct fixture_element A_L2[1]  = {{1,0}};              /* CPE0 */
static const struct fixture_element A_L3a[2] = {{1,0},{0,0}};        /* CPE0 SCE0 */
static const struct fixture_element A_L3b[2] = {{0,0},{1,0}};        /* SCE0 CPE0 */

static void run_invalid_updates(struct inst *a, const struct asc_spec *la1) {
    unsigned char asc[80]; size_t n; struct asc_spec s;
    /* channelConfiguration != 0 */
    s = *la1; s.chconf = 2;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* LFE element present */
    s = *la1; s.nlfe = 1; s.lfe_tag[0] = 0;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* duplicate (type, tag) */
    asc_spec_init(&s); s.nfront = 2;
    s.front[0].pair = 0; s.front[0].tag = 0;
    s.front[1].pair = 0; s.front[1].tag = 0;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* no front element */
    asc_spec_init(&s); s.nside = 1; s.side[0].pair = 0; s.side[0].tag = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* more than 8 channels */
    asc_spec_init(&s); s.nfront = 5;
    s.front[0].pair = 0; s.front[0].tag = 0;
    s.front[1].pair = 1; s.front[1].tag = 1;
    s.front[2].pair = 1; s.front[2].tag = 2;
    s.front[3].pair = 1; s.front[3].tag = 3;
    s.front[4].pair = 1; s.front[4].tag = 4;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* sample rate changed */
    s = *la1; s.sf_index = 4;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* PCE profile does not match the ASC */
    s = *la1; s.pce_profile = 0;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* PCE sample rate does not match the ASC */
    s = *la1; s.pce_sf = 4;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* truncated ASC */
    n = layout_asc(asc, sizeof asc, la1); TASK_CHECK(n > 1);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n-1) == -1);
    /* associated data element */
    s = *la1; s.nassoc = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* coupling element */
    s = *la1; s.ncc = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* mixdowns */
    s = *la1; s.mono_mix = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    s = *la1; s.stereo_mix = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    s = *la1; s.matrix_mix = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* 960 sample frames */
    s = *la1; s.framelen_flag = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* extension flag */
    s = *la1; s.ext_flag = 1;
    n = layout_asc(asc, sizeof asc, &s); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, (unsigned long)n) == -1);
    /* bad arguments */
    n = layout_asc(asc, sizeof asc, la1); TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, NULL, (unsigned long)n) == -1);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, 0) == -1);
    TASK_CHECK(NeAACDecUpdatePCE(a->h, asc, 1) == -1);
    TASK_CHECK(NeAACDecUpdatePCE(NULL, asc, (unsigned long)n) == -1);
    /* the failed updates must not have touched the active layout */
    TASK_CHECK(a->st->pce_sce_mask == (1u<<9));
    TASK_CHECK(a->st->pce_cpe_mask == (1u<<0));
    TASK_CHECK(a->st->pce.channels == 3);
    TASK_CHECK(a->st->frame == 12);
}

static void case_main_and_interleaved(void) {
    struct inst a, b;
    struct ref ra_cpe0, ra_sce0, ra_sce9, ra_sce0b;
    struct ref rb_cpe1, rb_sce2, rb_sce3, rb_sce4;
    struct asc_spec la1, la2, la3, lb0, lb1;
    unsigned char asc0[64], asc1[64], ascx[64];
    size_t n0, n1;
    unsigned f;

    static const struct fixture_element B_L0[3] = {{1,1},{0,2},{0,3}};
    static const struct fixture_element B_O4[3] = {{0,4},{0,2},{1,1}};
    static const struct fixture_element B_O5[3] = {{1,1},{0,4},{0,2}};
    static const struct fixture_element B_O6[3] = {{0,2},{1,1},{0,4}};
    static const struct fixture_element B_O7[3] = {{1,1},{0,2},{0,4}};

    /* instance A: SCE0 + CPE0 */
    n0 = fixture_asc(asc0, sizeof asc0, A_L0, 2);
    TASK_CHECK(n0 > 0);
    inst_open(&a, asc0, (unsigned long)n0);
    spec_front(&la1, A_L1a, 2);
    spec_front(&la2, A_L2, 1);
    spec_front(&la3, A_L3a, 2);
    n1 = layout_asc(asc1, sizeof asc1, &la1);
    TASK_CHECK(n1 > 0);

    /* instance B: front CPE1, side SCE2, back SCE3 */
    asc_spec_init(&lb0);
    lb0.nfront = 1; lb0.front[0].pair = 1; lb0.front[0].tag = 1;
    lb0.nside  = 1; lb0.side[0].pair  = 0; lb0.side[0].tag  = 2;
    lb0.nback  = 1; lb0.back[0].pair  = 0; lb0.back[0].tag  = 3;
    n0 = layout_asc(ascx, sizeof ascx, &lb0);
    TASK_CHECK(n0 > 0);
    inst_open(&b, ascx, (unsigned long)n0);
    asc_spec_init(&lb1);
    lb1.nfront = 1; lb1.front[0].pair = 0; lb1.front[0].tag = 4;
    lb1.nside  = 1; lb1.side[0].pair  = 0; lb1.side[0].tag  = 2;
    lb1.nback  = 1; lb1.back[0].pair  = 1; lb1.back[0].tag  = 1;

    ref_open1(&ra_cpe0, 1, 0);
    ref_open1(&ra_sce0, 0, 0);
    ref_open1(&ra_sce9, 0, 9);
    ref_open1(&ra_sce0b, 0, 0);
    ref_open1(&rb_cpe1, 1, 1);
    ref_open1(&rb_sce2, 0, 2);
    ref_open1(&rb_sce3, 0, 3);
    ref_open1(&rb_sce4, 0, 4);

    for(f=0; f<19; ++f) {
        unsigned seq = FSEQ(f), shape = FSHAPE(f), gain = GAIN_A(f);

        /* ---- instance A frame ---- */
        if(f == 6) {
            /* SCE0 + CPE0 -> CPE0 + SCE9 between two complete frames */
            TASK_CHECK(NeAACDecUpdatePCE(a.h, asc1, (unsigned long)n1) == 0);
            /* the caller's buffer is not retained */
            memset(asc1, 0xAB, n1);
            TASK_CHECK(a.st->frame == 6);
        }
        if(f == 11) {
            /* same-value update must not reset any history */
            n1 = layout_asc(asc1, sizeof asc1, &la1);
            TASK_CHECK(n1 > 0);
            TASK_CHECK(NeAACDecUpdatePCE(a.h, asc1, (unsigned long)n1) == 0);
            TASK_CHECK(a.st->frame == 11);
        }
        if(f == 12) run_invalid_updates(&a, &la1);
        if(f == 13) {
            /* reduce to CPE0 only */
            n1 = layout_asc(asc1, sizeof asc1, &la2);
            TASK_CHECK(n1 > 0);
            TASK_CHECK(NeAACDecUpdatePCE(a.h, asc1, (unsigned long)n1) == 0);
        }
        if(f == 16) {
            /* bring SCE0 back: it is a new identity, not a revival */
            n1 = layout_asc(asc1, sizeof asc1, &la3);
            TASK_CHECK(n1 > 0);
            TASK_CHECK(NeAACDecUpdatePCE(a.h, asc1, (unsigned long)n1) == 0);
        }

        if(f < 6) inst_decode(&a, A_L0, 2, seq, shape, gain);
        else if(f == 6 || f == 9 || f == 11) inst_decode(&a, A_L1a, 2, seq, shape, gain);
        else if(f < 13) inst_decode(&a, A_L1b, 2, seq, shape, gain);
        else if(f < 16) inst_decode(&a, A_L2, 1, seq, shape, gain);
        else if(f == 17) inst_decode(&a, A_L3b, 2, seq, shape, gain);
        else inst_decode(&a, A_L3a, 2, seq, shape, gain);

        if(f == 0) TASK_CHECK(a.samples == 0); /* genuine first frame only */
        else TASK_CHECK(a.samples == 1024ul*a.nch);

        if(f == 6) {
            TASK_CHECK(a.info.channels == 3);
            TASK_CHECK(a.info.num_front_channels == 3);
            TASK_CHECK(a.info.num_side_channels == 0);
            TASK_CHECK(a.info.num_back_channels == 0);
            TASK_CHECK(a.info.channel_position[0] == FRONT_CHANNEL_CENTER);
            TASK_CHECK(a.info.channel_position[1] == FRONT_CHANNEL_LEFT);
            TASK_CHECK(a.info.channel_position[2] == FRONT_CHANNEL_RIGHT);
        }
        if(f == 13) TASK_CHECK(a.info.channels == 2);
        if(f == 16) TASK_CHECK(a.info.channels == 3);

        /* references for instance A */
        ref_step(&ra_cpe0, seq, shape, gain);
        ref_step(&ra_sce0, seq, shape, gain);
        if(f >= 6 && f <= 12) ref_step(&ra_sce9, seq, shape, gain);
        if(f >= 16) ref_step(&ra_sce0b, seq, shape, gain);

        if(f < 6) {
            check_ele(&a, 0, &ra_sce0);
            check_ele(&a, 1, &ra_cpe0);
        } else if(f < 13) {
            check_ele(&a, 0, &ra_cpe0);
            check_ele(&a, 2, &ra_sce9);
        } else if(f < 16) {
            check_ele(&a, 0, &ra_cpe0);
        } else {
            check_ele(&a, 0, &ra_cpe0);
            check_ele(&a, 2, &ra_sce0b);
            if(f == 16) {
                /* the retired SCE0 tail must not come back: the re-added
                   identity starts from a clean synthesis history */
                TASK_CHECK(!reals_close(a.st->time_out[2],
                                        ra_sce0.st->time_out[0], 1024));
            }
        }

        /* ---- instance B frame (interleaved with A) ---- */
        if(f < 8) {
            unsigned bseq = FSEQ(f), bshape = FSHAPE(f), bgain = GAIN_B(f);
            if(f == 4) {
                n0 = layout_asc(ascx, sizeof ascx, &lb1);
                TASK_CHECK(n0 > 0);
                TASK_CHECK(NeAACDecUpdatePCE(b.h, ascx, (unsigned long)n0) == 0);
                TASK_CHECK(b.st->frame == 4);
            }
            if(f < 4) inst_decode(&b, B_L0, 3, bseq, bshape, bgain);
            else if(f == 4) inst_decode(&b, B_O4, 3, bseq, bshape, bgain);
            else if(f == 5) inst_decode(&b, B_O5, 3, bseq, bshape, bgain);
            else if(f == 6) inst_decode(&b, B_O6, 3, bseq, bshape, bgain);
            else inst_decode(&b, B_O7, 3, bseq, bshape, bgain);

            if(f == 0) TASK_CHECK(b.samples == 0);
            else TASK_CHECK(b.samples == 1024ul*b.nch);
            TASK_CHECK(b.info.channels == 4);

            if(f == 0) {
                TASK_CHECK(b.info.num_front_channels == 2);
                TASK_CHECK(b.info.num_side_channels == 1);
                TASK_CHECK(b.info.num_back_channels == 1);
                TASK_CHECK(b.info.channel_position[0] == FRONT_CHANNEL_LEFT);
                TASK_CHECK(b.info.channel_position[1] == FRONT_CHANNEL_RIGHT);
                TASK_CHECK(b.info.channel_position[2] == SIDE_CHANNEL_LEFT);
                TASK_CHECK(b.info.channel_position[3] == BACK_CHANNEL_CENTER);
            }
            if(f == 4) {
                TASK_CHECK(b.info.num_front_channels == 1);
                TASK_CHECK(b.info.num_side_channels == 1);
                TASK_CHECK(b.info.num_back_channels == 2);
                TASK_CHECK(b.info.channel_position[0] == FRONT_CHANNEL_CENTER);
                TASK_CHECK(b.info.channel_position[1] == SIDE_CHANNEL_LEFT);
                TASK_CHECK(b.info.channel_position[2] == BACK_CHANNEL_LEFT);
                TASK_CHECK(b.info.channel_position[3] == BACK_CHANNEL_RIGHT);
            }

            ref_step(&rb_cpe1, bseq, bshape, bgain);
            ref_step(&rb_sce2, bseq, bshape, bgain);
            if(f < 4) ref_step(&rb_sce3, bseq, bshape, bgain);
            else ref_step(&rb_sce4, bseq, bshape, bgain);

            if(f < 4) {
                check_ele(&b, 0, &rb_cpe1);
                check_ele(&b, 2, &rb_sce2);
                check_ele(&b, 3, &rb_sce3);
            } else {
                check_ele(&b, 0, &rb_sce4);
                check_ele(&b, 1, &rb_sce2);
                check_ele(&b, 2, &rb_cpe1);
            }
        }
    }

    NeAACDecClose(a.h);
    NeAACDecClose(b.h);
    ref_close(&ra_cpe0); ref_close(&ra_sce0); ref_close(&ra_sce9); ref_close(&ra_sce0b);
    ref_close(&rb_cpe1); ref_close(&rb_sce2); ref_close(&rb_sce3); ref_close(&rb_sce4);
}

/* decode-time rules of a locked layout on a scratch instance */
static void case_locked_decode_rules(void) {
    static const struct fixture_element L1a[2] = {{1,0},{0,9}};
    static const struct fixture_element L1b[2] = {{0,9},{1,0}};
    static const struct fixture_element ONLY_CPE0[1] = {{1,0}};
    static const struct fixture_element ONLY_SCE9[1] = {{0,9}};
    static const struct fixture_element BAD_SCE5[2] = {{1,0},{0,5}};
    static const struct fixture_element BAD_SCE0[2] = {{0,0},{0,9}};
    static const struct fixture_element BAD_DUP[3] = {{1,0},{1,0},{0,9}};
    struct asc_spec la1;
    unsigned char asc[64], pkt[256];
    size_t n, z;
    struct inst c;
    unsigned g = 120;

    spec_front(&la1, L1a, 2);
    n = layout_asc(asc, sizeof asc, &la1);
    TASK_CHECK(n > 0);
    inst_open(&c, asc, (unsigned long)n);

    inst_decode(&c, L1a, 2, 0, 0, g);
    inst_decode(&c, L1a, 2, 1, 1, g+3);
    TASK_CHECK(NeAACDecUpdatePCE(c.h, asc, (unsigned long)n) == 0);

    /* reordered elements decode fine once locked */
    inst_decode(&c, L1b, 2, 2, 0, g+5);
    TASK_CHECK(c.samples == 3072);

    /* missing element */
    inst_decode_error(&c, ONLY_CPE0, 1, 3, 1, g+7);
    inst_decode(&c, L1a, 2, 0, 0, g+9);
    /* unknown SCE tag */
    inst_decode_error(&c, BAD_SCE5, 2, 1, 0, g+11);
    inst_decode(&c, L1b, 2, 2, 1, g+13);
    /* SCE carrying the tag of the CPE identity */
    inst_decode_error(&c, BAD_SCE0, 2, 3, 0, g+15);
    inst_decode(&c, L1a, 2, 0, 1, g+17);
    /* duplicated element in one access unit */
    inst_decode_error(&c, BAD_DUP, 3, 1, 1, g+19);
    inst_decode(&c, L1b, 2, 2, 0, g+21);
    /* missing the CPE */
    inst_decode_error(&c, ONLY_SCE9, 1, 3, 1, g+23);
    inst_decode(&c, L1a, 2, 0, 0, g+25);

    /* LFE element in the access unit */
    z = fixture_packet(pkt, sizeof pkt, ONLY_SCE9, 1, 0, 0, g+27);
    TASK_CHECK(z > 0);
    pkt[0] = (unsigned char)((pkt[0]&0x1F) | (3<<5)); /* ID_LFE */
    TASK_CHECK(NeAACDecDecode(c.h, &c.info, pkt, (unsigned long)z) == NULL);
    TASK_CHECK(c.info.error != 0);
    inst_decode(&c, L1b, 2, 1, 1, g+29);
    TASK_CHECK(c.samples == 3072);

    /* an in-stream PCE stays ignored and must not clobber the layout */
    {
        struct asc_spec foreign;
        asc_spec_init(&foreign);
        foreign.nfront = 1; foreign.front[0].pair = 0; foreign.front[0].tag = 0;
        z = layout_packet_pce(pkt, sizeof pkt, &foreign, L1a, 2, 2, 0, g+31);
        TASK_CHECK(z > 0);
        TASK_CHECK(NeAACDecDecode(c.h, &c.info, pkt, (unsigned long)z) != NULL);
        TASK_CHECK(c.info.error == 0);
        TASK_CHECK(c.info.samples == 3072);
        ++c.frame;
        TASK_CHECK(c.st->frame == c.frame);
        TASK_CHECK(c.st->pce_sce_mask == (1u<<9));
        TASK_CHECK(c.st->pce_cpe_mask == (1u<<0));
        TASK_CHECK(c.st->pce.channels == 3);
    }
    inst_decode(&c, L1b, 2, 3, 1, g+33);
    TASK_CHECK(c.samples == 3072);

    NeAACDecClose(c.h);
}

/* the update is only offered to explicit-PCE Init2 handles */
static void case_handle_restrictions(void) {
    static const struct fixture_element L1a[2] = {{1,0},{0,9}};
    struct asc_spec la1, cc2;
    unsigned char asc[64];
    size_t n;
    NeAACDecHandle h;
    unsigned long rate; unsigned char ch;

    spec_front(&la1, L1a, 2);
    n = layout_asc(asc, sizeof asc, &la1);
    TASK_CHECK(n > 0);

    /* never initialised */
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)n) == -1);
    NeAACDecClose(h);

    /* initialised without an explicit PCE */
    asc_spec_init(&cc2); cc2.chconf = 2;
    n = layout_asc(asc, sizeof asc, &cc2);
    TASK_CHECK(n > 0);
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    rate = 0; ch = 0;
    TASK_CHECK(NeAACDecInit2(h, asc, (unsigned long)n, &rate, &ch) == 0);
    TASK_CHECK(ch == 2);
    n = layout_asc(asc, sizeof asc, &la1);
    TASK_CHECK(n > 0);
    TASK_CHECK(NeAACDecUpdatePCE(h, asc, (unsigned long)n) == -1);
    NeAACDecClose(h);
}

void task_cases(void) {
    case_main_and_interleaved();
    case_locked_decode_rules();
    case_handle_restrictions();
    puts("layout update cases done");
}
