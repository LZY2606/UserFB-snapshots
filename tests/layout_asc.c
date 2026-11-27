#include "layout_asc.h"
#include <string.h>

struct asc_writer { unsigned char *out; size_t cap, bits; int failed; };

static void asc_put(struct asc_writer *w, unsigned value, unsigned n)
{
    while (n--) {
        if (w->bits/8 >= w->cap) w->failed = 1;
        else w->out[w->bits/8] |= ((value>>n)&1u) << (7-w->bits%8);
        ++w->bits;
    }
}

static void asc_align(struct asc_writer *w) { while (w->bits%8) asc_put(w,0,1); }

void layout_asc_defaults(struct layout_asc_spec *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->profile = 2;       /* AAC LC */
    sp->sf_index = 3;      /* 48 kHz */
    sp->chan_cfg = 0;      /* explicit PCE */
    sp->frame_len_flag = 0;/* 1024 samples */
    sp->pce_profile = 1;   /* LC in the PCE two bit profile field */
    sp->pce_sf_index = 3;
    sp->front[0].pair = 0; sp->front[0].tag = 0; /* SCE0 */
    sp->front[1].pair = 1; sp->front[1].tag = 0; /* CPE0 */
    sp->n_front = 2;
}

static void asc_put_elements(struct asc_writer *w,
                             const struct fixture_element *e, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) {
        asc_put(w, e[i].pair, 1);
        asc_put(w, e[i].tag, 4);
    }
}

size_t layout_asc_write(unsigned char *out, size_t cap,
                        const struct layout_asc_spec *sp)
{
    struct asc_writer w = {out, cap, 0, 0};
    unsigned i;
    if (!out || !cap || !sp) return 0;
    if (sp->n_front > 16 || sp->n_side > 16 || sp->n_back > 16 || sp->n_lfe > 4)
        return 0;
    memset(out, 0, cap);
    asc_put(&w, sp->profile, 5);
    asc_put(&w, sp->sf_index, 4);
    asc_put(&w, sp->chan_cfg, 4);
    asc_put(&w, sp->frame_len_flag, 1);
    asc_put(&w, 0, 1); /* dependsOnCoreCoder */
    asc_put(&w, 0, 1); /* extensionFlag */
    if (sp->chan_cfg == 0) {
        asc_put(&w, 0, 4);              /* PCE element_instance_tag */
        asc_put(&w, sp->pce_profile, 2);
        asc_put(&w, sp->pce_sf_index, 4);
        asc_put(&w, sp->n_front, 4);
        asc_put(&w, sp->n_side, 4);
        asc_put(&w, sp->n_back, 4);
        asc_put(&w, sp->n_lfe, 2);
        asc_put(&w, sp->n_assoc, 3);
        asc_put(&w, sp->n_cc, 4);
        asc_put(&w, sp->mono_mix, 1);
        if (sp->mono_mix) asc_put(&w, 0, 4);
        asc_put(&w, sp->stereo_mix, 1);
        if (sp->stereo_mix) asc_put(&w, 0, 4);
        asc_put(&w, sp->matrix_mix, 1);
        if (sp->matrix_mix) { asc_put(&w, 0, 2); asc_put(&w, 0, 1); }
        asc_put_elements(&w, sp->front, sp->n_front);
        asc_put_elements(&w, sp->side, sp->n_side);
        asc_put_elements(&w, sp->back, sp->n_back);
        for (i = 0; i < sp->n_lfe; ++i) asc_put(&w, sp->lfe[i], 4);
        for (i = 0; i < sp->n_assoc; ++i) asc_put(&w, 0, 4);
        for (i = 0; i < sp->n_cc; ++i) { asc_put(&w, 0, 1); asc_put(&w, 0, 4); }
        asc_align(&w);
        asc_put(&w, 0, 8); /* no comment */
    }
    return w.failed ? 0 : (w.bits+7)/8;
}
