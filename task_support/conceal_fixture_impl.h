/* Header-only raw AAC-LC fixture builder for tests/conceal.c.
   The task build links only driver.c and tests/conceal.c, so the generator
   lives here rather than in a separate translation unit. */
#ifndef TASK_CONCEAL_FIXTURE_IMPL_H
#define TASK_CONCEAL_FIXTURE_IMPL_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    unsigned char *data;
    size_t capacity;
    size_t bits;
} fixture_bw;

typedef struct
{
    int channels;              /* 1 (SCE) or 2 (CPE) */
    int window_sequence;       /* 0..3 */
    int window_shape;          /* 0 = sine, 1 = KBD (sine is used) */
    int common_window;         /* CPE only */
    int ms_mask_present;       /* CPE only: 0/1/2 */
    int tns;                   /* 0 = off, 1 = short TNS, 2 = long TNS */
    int noise_start_sfb;       /* >= 0: PNS from this sfb onwards */
    int intensity_start_sfb;   /* >= 0: IS from this sfb onwards */
    int noise_energy;
    int gain;                  /* global gain */
    int value;                 /* 0 = zero pairs, 1 = (1,1) pairs */
} fixture_au_desc;


#include <stdlib.h>
#include <string.h>

#define CB6_CODE_11 10u, 4  /* 1010: (1,1), 4-bit code */
#define CB6_CODE_00 0u, 4   /* 0000: (0,0), 4-bit code */

#define SWB_LONG_USED 12
#define SWB_SHORT_USED 8

/* selected AAC swb offsets for 48 kHz (long and short windows) */
static const unsigned short swb_1024_48[] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72,
    80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292,
    320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736,
    768, 800, 832, 864, 896, 928, 1024
};
static const unsigned short swb_128_48[] = {
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128
};

/* public bit writer ------------------------------------------------------- */

static void fixture_bw_init(fixture_bw *bw, size_t capacity)
{
    bw->data = (unsigned char*)calloc(capacity ? capacity : 1, 1);
    bw->capacity = capacity;
    bw->bits = 0;
}

static void fixture_bw_free(fixture_bw *bw)
{
    free(bw->data);
    bw->data = NULL;
    bw->capacity = 0;
    bw->bits = 0;
}

static void fixture_bw_put(fixture_bw *bw, unsigned value, unsigned nbits)
{
    unsigned i;

    for (i = 0; i < nbits; i++)
    {
        unsigned bit = (value >> (nbits - 1 - i)) & 1u;
        size_t byte = bw->bits >> 3;

        if (byte >= bw->capacity)
            return;
        if (bit)
            bw->data[byte] |= (unsigned char)(0x80u >> (bw->bits & 7));
        else
            bw->data[byte] &= (unsigned char)~(0x80u >> (bw->bits & 7));
        bw->bits++;
    }
}

static void fixture_bw_align(fixture_bw *bw)
{
    while (bw->bits & 7)
        fixture_bw_put(bw, 0, 1);
}

static size_t fixture_bw_bytes(const fixture_bw *bw)
{
    return (bw->bits + 7) / 8;
}

/* builder ---------------------------------------------------------------- */

typedef struct
{
    fixture_bw *bw;
    int short_window;
    int used_sfb;
    int noise_sfb;       /* -1 when no PNS */
    int intensity_sfb;   /* -1 when no IS */
    int tns;
    int gain;
    int noise_energy;
    int value;           /* spectral magnitude: 0 = zero pairs, 1 = (1,1) */
} au_builder;

/* one section per group; spectral codebook for the bands before the noise
   border, then one NOISE_HCB section for the rest */
static void emit_section_data(au_builder *b, int section_bits)
{
    int border = b->used_sfb;
    int esc = (1 << section_bits) - 1;
    int max_len = esc - 1;                 /* never emit the escape value */
    int pos = 0;

    if (b->noise_sfb >= 0 && b->noise_sfb < border)
        border = b->noise_sfb;

    /* spectral bands: one or more sections with the same codebook */
    while (pos < border)
    {
        int len = border - pos;
        if (len > max_len)
            len = max_len;
        fixture_bw_put(b->bw, 6, 4);                     /* codebook 6 */
        fixture_bw_put(b->bw, (unsigned)len, (unsigned)section_bits);
        pos += len;
    }

    /* PNS bands: a single NOISE_HCB range, split if it cannot be represented */
    if (b->noise_sfb >= 0 && b->noise_sfb < b->used_sfb)
    {
        pos = b->noise_sfb;
        while (pos < b->used_sfb)
        {
            int len = b->used_sfb - pos;
            if (len > max_len)
                len = max_len;
            fixture_bw_put(b->bw, 13, 4);                /* NOISE_HCB */
            fixture_bw_put(b->bw, (unsigned)len, (unsigned)section_bits);
            pos += len;
        }
    }
}

static void emit_scale_factors(au_builder *b)
{
    int sfb;
    int first_noise = 1;

    for (sfb = 0; sfb < b->used_sfb; sfb++)
    {
        if (b->noise_sfb >= 0 && sfb >= b->noise_sfb)
        {
            if (first_noise)
            {
                first_noise = 0;
                /* first noise energy is a raw 9-bit value */
                fixture_bw_put(b->bw, (unsigned)(b->noise_energy + 256), 9);
            }
            else
            {
                fixture_bw_put(b->bw, 0, 1);                   /* delta 0 */
            }
        }
        else if (b->intensity_sfb >= 0 && sfb >= b->intensity_sfb)
        {
            fixture_bw_put(b->bw, 0, 1);                       /* position delta 0 */
        }
        else
        {
            fixture_bw_put(b->bw, 0, 1);                       /* delta 0 */
        }
    }
}

static void emit_spectral_data(au_builder *b)
{
    int sfb;
    int limit = b->used_sfb;
    int groups = b->short_window ? 8 : 1;
    int g;

    if (b->noise_sfb >= 0 && b->noise_sfb < limit)
        limit = b->noise_sfb;
    if (b->intensity_sfb >= 0 && b->intensity_sfb < limit)
        limit = b->intensity_sfb;

    /* one spectral pass per window group (a single window per group here) */
    for (g = 0; g < groups; g++)
    {
        for (sfb = 0; sfb < limit; sfb++)
        {
            int width = b->short_window
                ? swb_128_48[sfb+1] - swb_128_48[sfb]
                : swb_1024_48[sfb+1] - swb_1024_48[sfb];
            int i;

            for (i = 0; i < width; i += 2)
            {
                /* codebook 6 is unsigned: (1,1) followed by two sign bits, or
                   the all-zero pair which carries no sign bits */
                if (b->value)
                {
                    fixture_bw_put(b->bw, CB6_CODE_11);
                    fixture_bw_put(b->bw, 0, 1);
                    fixture_bw_put(b->bw, 0, 1);
                }
                else
                {
                    fixture_bw_put(b->bw, CB6_CODE_00);
                }
            }
        }
    }
}

static void emit_tns(au_builder *b)
{
    int w;
    int windows = b->short_window ? 8 : 1;

    if (!b->tns)
    {
        fixture_bw_put(b->bw, 0, 1);
        return;
    }
    fixture_bw_put(b->bw, 1, 1);            /* tns_data_present */
    for (w = 0; w < windows; w++)
    {
        if (b->short_window)
        {
            /* one filter in the first window only */
            if (w == 0)
            {
                fixture_bw_put(b->bw, 1, 1);    /* n_filt */
                fixture_bw_put(b->bw, 1, 1);    /* coef_res */
                fixture_bw_put(b->bw, 1, 4);    /* length */
                fixture_bw_put(b->bw, 1, 3);    /* order */
                fixture_bw_put(b->bw, 0, 1);    /* direction */
                fixture_bw_put(b->bw, 1, 1);    /* coef_compress */
                fixture_bw_put(b->bw, 1, 3);    /* coef */
            }
            else
                fixture_bw_put(b->bw, 0, 1);    /* n_filt = 0 */
        }
        else
        {
            fixture_bw_put(b->bw, 1, 2);        /* n_filt */
            fixture_bw_put(b->bw, 0, 1);        /* coef_res */
            fixture_bw_put(b->bw, 2, 6);        /* length */
            fixture_bw_put(b->bw, 1, 5);        /* order */
            fixture_bw_put(b->bw, 0, 1);        /* direction */
            fixture_bw_put(b->bw, 0, 1);        /* coef_compress */
            fixture_bw_put(b->bw, 1, 3);        /* coef */
        }
    }
}

static void emit_window_info(au_builder *b)
{
    fixture_bw_put(b->bw, 0, 1);                              /* reserved */
    fixture_bw_put(b->bw, b->short_window ? 2u : 0u, 2);      /* ONLY_LONG/EIGHT_SHORT */
    fixture_bw_put(b->bw, 0, 1);                              /* window_shape: sine */
    if (b->short_window)
    {
        fixture_bw_put(b->bw, (unsigned)b->used_sfb, 4);
        fixture_bw_put(b->bw, 0, 7);                          /* one group of 8 */
        /* no predictor_data_present bit for short windows */
    }
    else
    {
        fixture_bw_put(b->bw, (unsigned)b->used_sfb, 6);
        fixture_bw_put(b->bw, 0, 1);                          /* predictor */
    }
}

/* one channel: gain, window info (unless common), sections, scale factors,
   pulse/TNS/gain-control flags and the spectral data */
static void emit_channel(au_builder *b)
{
    int groups = b->short_window ? 8 : 1;
    int g;

    fixture_bw_put(b->bw, (unsigned)b->gain, 8);
    emit_window_info(b);
    for (g = 0; g < groups; g++)
        emit_section_data(b, b->short_window ? 3 : 5);
    for (g = 0; g < groups; g++)
        emit_scale_factors(b);
    fixture_bw_put(b->bw, 0, 1);            /* pulse_data_present */
    emit_tns(b);
    fixture_bw_put(b->bw, 0, 1);            /* gain_control_data_present */
    emit_spectral_data(b);
}

/* channel of a common-window CPE: no window info (shared) */
static void emit_channel_shared(au_builder *b)
{
    int groups = b->short_window ? 8 : 1;
    int g;

    fixture_bw_put(b->bw, (unsigned)b->gain, 8);
    for (g = 0; g < groups; g++)
        emit_section_data(b, b->short_window ? 3 : 5);
    for (g = 0; g < groups; g++)
        emit_scale_factors(b);
    fixture_bw_put(b->bw, 0, 1);
    emit_tns(b);
    fixture_bw_put(b->bw, 0, 1);
    emit_spectral_data(b);
}

static void init_builder(au_builder *b, fixture_bw *bw, int short_window,
                         const fixture_au_desc *desc)
{
    memset(b, 0, sizeof(*b));
    b->bw = bw;
    b->short_window = short_window;
    b->used_sfb = short_window ? SWB_SHORT_USED : SWB_LONG_USED;
    b->noise_sfb = desc->noise_start_sfb;
    b->intensity_sfb = desc->intensity_start_sfb;
    b->tns = desc->tns;
    b->gain = desc->gain;
    b->noise_energy = desc->noise_energy;
    b->value = desc->value;
}

static int fixture_make_au(fixture_bw *bw, const fixture_au_desc *desc)
{
    au_builder b;
    int short_window;

    if (bw == NULL || desc == NULL ||
        (desc->channels != 1 && desc->channels != 2))
        return -1;
    if (desc->window_sequence < 0 || desc->window_sequence > 2)
        return -1;

    short_window = (desc->window_sequence == 2);
    init_builder(&b, bw, short_window, desc);
    if (desc->noise_start_sfb > b.used_sfb)
        b.noise_sfb = -1;
    if (desc->intensity_start_sfb > b.used_sfb)
        b.intensity_sfb = -1;

    fixture_bw_put(bw, desc->channels == 1 ? 0u : 1u, 3);
    fixture_bw_put(bw, 0, 4);                       /* element_instance_tag */

    if (desc->channels == 1)
    {
        emit_channel(&b);
    }
    else if (desc->common_window)
    {
        fixture_bw_put(bw, 1, 1);
        emit_window_info(&b);
        fixture_bw_put(bw, (unsigned)desc->ms_mask_present, 2);
        if (desc->ms_mask_present == 1)
        {
            int sfb;
            for (sfb = 0; sfb < b.used_sfb; sfb++)
                fixture_bw_put(bw, 1, 1);
        }
        emit_channel_shared(&b);
        emit_channel_shared(&b);
    }
    else
    {
        fixture_bw_put(bw, 0, 1);
        emit_channel(&b);
        emit_channel(&b);
    }

    fixture_bw_put(bw, 7, 3);                       /* ID_END */
    fixture_bw_align(bw);
    return 0;
}

#endif
