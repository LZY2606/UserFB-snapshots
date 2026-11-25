/*
 * Concealment tests for NeAACDecConceal() / NeAACDecConceal2().
 *
 * The numerical oracle owns an independent filterbank and overlap history.
 * It is initialised once from a good AU's real post-tool/pre-filter spectrum
 * and the live overlap, then advanced independently for every missing AU.
 */

#define HAVE_INTTYPES_H 1
#define HAVE_STRING_H 1

#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"
#include "common.h"
#include "structs.h"
#include "filtbank.h"
#include "syntax.h"

#define AU_SAMPLES 1024u
#define MAX_FIXTURE_AUS 64u

typedef struct
{
    unsigned char *data;
    size_t size;
    unsigned count;
    size_t offset[MAX_FIXTURE_AUS];
    unsigned long length[MAX_FIXTURE_AUS];
} Fixture;

static int fixture_load(Fixture *fx)
{
    FILE *f;
    long loaded;
    size_t at;

    memset(fx, 0, sizeof(*fx));
    f = fopen("task_support/normal.aac", "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    loaded = ftell(f);
    if (loaded <= 7)
    {
        fclose(f);
        return 0;
    }
    rewind(f);
    fx->size = (size_t)loaded;
    fx->data = (unsigned char*)malloc(fx->size);
    if (!fx->data || fread(fx->data, 1, fx->size, f) != fx->size)
    {
        free(fx->data);
        fclose(f);
        return 0;
    }
    fclose(f);

    at = 0;
    while (at + 7 <= fx->size && fx->count < MAX_FIXTURE_AUS)
    {
        unsigned char *b = fx->data + at;
        unsigned len;

        if (b[0] != 0xff || (b[1] & 0xf6) != 0xf0)
            return 0;
        len = ((unsigned)(b[3] & 3) << 11) | ((unsigned)b[4] << 3) |
              ((unsigned)(b[5] >> 5));
        if (len < 8 || (size_t)len > fx->size - at)
            return 0;
        fx->offset[fx->count] = at + 7;
        fx->length[fx->count] = (unsigned long)len - 7;
        fx->count++;
        at += len;
    }
    return fx->count == 24 && at == fx->size;
}

static void fixture_free(Fixture *fx)
{
    free(fx->data);
    fx->data = NULL;
}

typedef struct
{
    int seen[CONCEAL_MAX_CHANNELS];
    uint8_t sequence[CONCEAL_MAX_CHANNELS];
    uint8_t shape[CONCEAL_MAX_CHANNELS];
    real_t spectrum[CONCEAL_MAX_CHANNELS][AU_SAMPLES];
} Observer;

static void test_observer(void *user, uint8_t channel,
                          uint8_t window_sequence, uint8_t window_shape,
                          const real_t *spectrum)
{
    Observer *o = (Observer*)user;
    if (!o || channel >= CONCEAL_MAX_CHANNELS)
        return;
    o->seen[channel] = 1;
    o->sequence[channel] = window_sequence;
    o->shape[channel] = window_shape;
    memcpy(o->spectrum[channel], spectrum, AU_SAMPLES * sizeof(real_t));
}

static NeAACDecHandle make_handle(unsigned char output_format,
                                  const unsigned char asc[2],
                                  Observer *observer,
                                  NeAACDecStruct **out_s)
{
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg;
    unsigned long rate = 0;
    unsigned char channels = 0;
    NeAACDecStruct *s;

    if (!h)
        return NULL;
    cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = output_format;
    cfg->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(h, cfg))
    {
        NeAACDecClose(h);
        return NULL;
    }
    if (NeAACDecInit2(h, (unsigned char*)asc, 2, &rate, &channels) != 0)
    {
        NeAACDecClose(h);
        return NULL;
    }
    s = (NeAACDecStruct*)h;
    if (observer)
    {
        s->conceal.observer = test_observer;
        s->conceal.observer_user = observer;
    }
    if (out_s)
        *out_s = s;
    return h;
}

static int decode_through(NeAACDecHandle h, const Fixture *fx, unsigned last)
{
    unsigned i;
    for (i = 0; i <= last; i++)
    {
        NeAACDecFrameInfo info;
        if (i >= fx->count)
            return 0;
        NeAACDecDecode(h, &info, fx->data + fx->offset[i], fx->length[i]);
        if (info.error)
        {
            fprintf(stderr, "fixture AU %u failed: %u\n", i, info.error);
            return 0;
        }
    }
    return 1;
}

static int check_rng(const NeAACDecStruct *s, uint32_t r1, uint32_t r2)
{
    if (s->__r1 != r1 || s->__r2 != r2)
    {
        fprintf(stderr, "RNG changed: %08x/%08x expected %08x/%08x\n",
                s->__r1, s->__r2, r1, r2);
        return 0;
    }
    return 1;
}

typedef struct
{
    fb_info *fb;
    uint8_t channels;
    real_t base[CONCEAL_MAX_CHANNELS][AU_SAMPLES];
    real_t overlap[CONCEAL_MAX_CHANNELS][AU_SAMPLES];
    real_t time[CONCEAL_MAX_CHANNELS][AU_SAMPLES];
    real_t work[CONCEAL_MAX_CHANNELS][AU_SAMPLES];
    uint8_t good_sequence[CONCEAL_MAX_CHANNELS];
    uint8_t good_shape[CONCEAL_MAX_CHANNELS];
    uint8_t last_sequence[CONCEAL_MAX_CHANNELS];
    uint8_t previous_shape[CONCEAL_MAX_CHANNELS];
} Oracle;

static uint8_t next_sequence(uint8_t sequence)
{
    switch (sequence)
    {
    case LONG_START_SEQUENCE: return EIGHT_SHORT_SEQUENCE;
    case EIGHT_SHORT_SEQUENCE: return EIGHT_SHORT_SEQUENCE;
    case LONG_STOP_SEQUENCE: return ONLY_LONG_SEQUENCE;
    default: return ONLY_LONG_SEQUENCE;
    }
}

static int geometry_short(uint8_t sequence)
{
    return sequence == EIGHT_SHORT_SEQUENCE;
}

static void oracle_init(Oracle *o, const NeAACDecStruct *s,
                        const Observer *observed, uint8_t channels)
{
    uint8_t ch;
    memset(o, 0, sizeof(*o));
    o->channels = channels;
    o->fb = filter_bank_init(AU_SAMPLES);
    for (ch = 0; ch < channels; ch++)
    {
        memcpy(o->base[ch], observed->spectrum[ch], sizeof(o->base[ch]));
        memcpy(o->overlap[ch], s->fb_intermed[ch], sizeof(o->overlap[ch]));
        o->good_sequence[ch] = observed->sequence[ch];
        o->good_shape[ch] = observed->shape[ch];
        o->last_sequence[ch] = observed->sequence[ch];
        o->previous_shape[ch] = s->window_shape_prev[ch];
    }
}

static void oracle_free(Oracle *o)
{
    if (o->fb)
        filter_bank_end(o->fb);
    o->fb = NULL;
}

static void oracle_make_spec(Oracle *o, uint8_t ch, unsigned long k)
{
    uint8_t target = next_sequence(o->last_sequence[ch]);
    unsigned i;
    double gain;

    o->last_sequence[ch] = target;
    if (k >= 32 ||
        geometry_short(o->good_sequence[ch]) != geometry_short(target))
    {
        memset(o->work[ch], 0, sizeof(o->work[ch]));
        return;
    }

    gain = pow(0.75, (double)k);
    for (i = 0; i < AU_SAMPLES; i++)
        o->work[ch][i] = (real_t)((double)o->base[ch][i] * gain);
}

static void oracle_run(Oracle *o, unsigned long k)
{
    uint8_t ch;
    for (ch = 0; ch < o->channels; ch++)
    {
        oracle_make_spec(o, ch, k);
        ifilter_bank(o->fb, o->last_sequence[ch], o->good_shape[ch],
                     o->previous_shape[ch], o->work[ch], o->time[ch],
                     o->overlap[ch], LC, AU_SAMPLES);
        o->previous_shape[ch] = o->good_shape[ch];
    }
}

/* Apply the reference spectrum prepared by the immediately preceding
   oracle_run() to another live decoder's real overlap. */
static void oracle_apply_to_handle(const Oracle *o, NeAACDecStruct *s)
{
    uint8_t out_ch;
    for (out_ch = 0; out_ch < o->channels; out_ch++)
    {
        uint8_t ch = s->internal_channel[out_ch];
        ifilter_bank(s->fb, o->last_sequence[out_ch], o->good_shape[out_ch],
                     s->window_shape_prev[ch], (real_t*)o->work[out_ch],
                     s->time_out[ch], s->fb_intermed[ch],
                     s->object_type, s->frameLength);
        s->window_shape_prev[ch] = o->good_shape[out_ch];
    }
    s->frame++;
}

static int close_ref(double got, double expected)
{
    double tolerance = 2e-5 * (1.0 + fabs(expected));
    double delta = fabs(got - expected);
    return delta <= tolerance;
}

static int verify_synthesis(const NeAACDecStruct *s, const Oracle *o,
                            const NeAACDecFrameInfo *info, const void *pcm,
                            unsigned char format, unsigned long expected_frame)
{
    uint8_t ch;
    unsigned i;
    unsigned long bad = 0;
    double worst = 0.0;

    if (info->error != 0 || info->bytesconsumed != 0)
    {
        fprintf(stderr, "bad conceal info error=%u consumed=%lu\n",
                info->error, info->bytesconsumed);
        return 0;
    }
    if (info->samples != AU_SAMPLES * o->channels ||
        info->channels != o->channels)
    {
        fprintf(stderr, "bad conceal samples=%lu channels=%u expected %u\n",
                info->samples, info->channels, o->channels);
        return 0;
    }
    if ((unsigned long)s->frame != expected_frame)
    {
        fprintf(stderr, "frame count %u expected %lu\n", s->frame, expected_frame);
        return 0;
    }

    for (ch = 0; ch < o->channels; ch++)
    {
        unsigned internal_ch = s->internal_channel[ch];
        if (s->window_shape_prev[internal_ch] != o->previous_shape[ch] ||
            s->conceal.synthesis_sequence[internal_ch] != o->last_sequence[ch])
        {
            fprintf(stderr, "window state mismatch ch%u: got shape %u seq %u\n",
                    ch, s->window_shape_prev[internal_ch],
                    s->conceal.synthesis_sequence[internal_ch]);
            return 0;
        }
        for (i = 0; i < AU_SAMPLES; i++)
        {
            double got = (double)s->fb_intermed[internal_ch][i];
            double expected = (double)o->overlap[ch][i];
            double delta = fabs(got - expected);
            if (delta > worst)
                worst = delta;
            if (!close_ref(got, expected))
            {
                if (bad < 5)
                    fprintf(stderr, "overlap mismatch ch%u i%u got %.9g ref %.9g d %.3g\n",
                            ch, i, got, expected, delta);
                bad++;
            }
        }
    }

    for (i = 0; i < AU_SAMPLES * o->channels; i++)
    {
        uint8_t ch = i % o->channels;
        unsigned sample = i / o->channels;
        double expected = (double)o->time[ch][sample] / 32768.0;
        double got;
        if (format == FAAD_FMT_FLOAT)
            got = ((const float*)pcm)[i];
        else
            got = (double)((const int16_t*)pcm)[i] / 32768.0;
        if (!close_ref(got, expected))
        {
            if (bad < 10)
                fprintf(stderr, "PCM mismatch i%u got %.9g ref %.9g\n",
                        i, got, expected);
            bad++;
        }
    }

    if (bad)
    {
        fprintf(stderr, "%lu mismatches, worst overlap delta %.3g\n", bad, worst);
        return 0;
    }
    return 1;
}

/* Minimal legal mono SCE raw AAC-LC frame generator.  max_sfb is either 0 or
   one PNS band; this gives genuinely nonzero post-PNS/TNS spectra without
   requiring a Huffman spectral encoder. */
typedef struct
{
    unsigned char bytes[128];
    unsigned bit;
} BitWriter;

static void bw_put(BitWriter *w, unsigned bits, unsigned value)
{
    int i;
    for (i = (int)bits - 1; i >= 0; i--)
    {
        unsigned byte = w->bit >> 3;
        unsigned place = 7 - (w->bit & 7);
        if ((value >> i) & 1u)
            w->bytes[byte] |= (unsigned char)(1u << place);
        w->bit++;
    }
}

static unsigned make_sce_au(unsigned char sequence, unsigned char shape,
                            int pns, int tns, unsigned char *out)
{
    BitWriter w;
    int short_block = sequence == EIGHT_SHORT_SEQUENCE;
    unsigned max_sfb = pns ? 1u : 0u;

    memset(&w, 0, sizeof(w));
    bw_put(&w, 3, ID_SCE);
    bw_put(&w, 4, 0);          /* element_instance_tag */
    bw_put(&w, 8, 140);        /* global_gain */

    bw_put(&w, 1, 0);          /* ics_reserved_bit */
    bw_put(&w, 2, sequence);
    bw_put(&w, 1, shape);
    if (short_block)
    {
        bw_put(&w, 4, max_sfb);
        bw_put(&w, 7, 0x7f);   /* all eight windows in one group */
    }
    else
    {
        bw_put(&w, 6, max_sfb);
    }
    if (!short_block)
        bw_put(&w, 1, 0);      /* predictor_data_present */

    if (pns)
    {
        /* One section covering one scalefactor band with NOISE_HCB (13). */
        bw_put(&w, 4, 13);
        bw_put(&w, short_block ? 3 : 5, 1);
        /* First PNS energy is a raw 9-bit signed-differential value. */
        bw_put(&w, 9, 210);
    }

    bw_put(&w, 1, 0);          /* pulse_data_present */
    bw_put(&w, 1, tns ? 1u : 0u);
    if (tns && !short_block)
    {
        bw_put(&w, 2, 1);      /* n_filt */
        bw_put(&w, 1, 0);      /* coef_res: 3 coefficient bits */
        bw_put(&w, 6, 63);     /* length: reach the single transmitted sfb */
        bw_put(&w, 5, 1);      /* order */
        bw_put(&w, 1, 0);      /* direction */
        bw_put(&w, 1, 0);      /* coef_compress */
        bw_put(&w, 3, 2);      /* one nonzero coefficient */
    }
    if (tns && short_block)
    {
        int win;
        for (win = 0; win < 8; win++)
        {
            bw_put(&w, 1, 1);
            bw_put(&w, 1, 0);
            bw_put(&w, 4, 15);
            bw_put(&w, 3, 1);
            bw_put(&w, 1, 0);
            bw_put(&w, 1, 0);
            bw_put(&w, 3, 2);
        }
    }
    bw_put(&w, 1, 0);          /* gain_control_data_present */
    bw_put(&w, 3, ID_END);
    while (w.bit & 7u)
        bw_put(&w, 1, 0);

    memcpy(out, w.bytes, (w.bit + 7u) / 8u);
    return (w.bit + 7u) / 8u;
}

static int decode_generated(NeAACDecHandle h, const unsigned char *aus,
                            const unsigned *lengths, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++)
    {
        NeAACDecFrameInfo info;
        NeAACDecDecode(h, &info, (unsigned char*)aus + i * 128, lengths[i]);
        if (info.error)
        {
            fprintf(stderr, "generated AU %u failed: %u\n", i, info.error);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------ spectrum ------------------------------ */

static int test_normal_stereo_spectrum(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    uint32_t r1, r2;
    unsigned long k;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 6));
    TASK_CHECK(obs.seen[0] && obs.seen[1]);
    TASK_CHECK(s->conceal.history_valid && s->conceal.channels == 2);
    TASK_CHECK(memcmp(s->conceal.spectrum[0], obs.spectrum[0],
                      sizeof(obs.spectrum[0])) == 0);
    TASK_CHECK(memcmp(s->conceal.spectrum[1], obs.spectrum[1],
                      sizeof(obs.spectrum[1])) == 0);

    oracle_init(&oracle, s, &obs, 2);
    r1 = s->__r1;
    r2 = s->__r2;
    for (k = 1; k <= 3; k++)
    {
        NeAACDecFrameInfo info;
        void *pcm;
        oracle_run(&oracle, k);
        pcm = NeAACDecConceal(h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT,
                                     7 + k));
        TASK_CHECK(info.samplerate == 48000 && info.object_type == LC);
        TASK_CHECK(info.num_front_channels == 2);
        TASK_CHECK(info.channel_position[0] == FRONT_CHANNEL_LEFT);
        TASK_CHECK(info.channel_position[1] == FRONT_CHANNEL_RIGHT);
        TASK_CHECK(check_rng(s, r1, r2));
        TASK_CHECK(s->conceal.lost_frames == k);
    }

    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_first_good_au_still_suppressed(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo first, conceal_info;
    void *pcm;

    memset(&obs, 0, sizeof(obs));
    memset(&first, 0, sizeof(first));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    NeAACDecDecode(h, &first, fx.data + fx.offset[0], fx.length[0]);
    TASK_CHECK(first.error == 0 && first.samples == 0);
    TASK_CHECK(s->conceal.history_valid);
    TASK_CHECK((unsigned long)s->frame == 1);

    oracle_init(&oracle, s, &obs, 2);
    oracle_run(&oracle, 1);
    pcm = NeAACDecConceal(h, &conceal_info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(conceal_info.samples == 2048 && conceal_info.error == 0);
    TASK_CHECK(verify_synthesis(s, &oracle, &conceal_info, pcm,
                                FAAD_FMT_FLOAT, 2));

    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_normal_long_start_to_short(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo info;
    void *pcm;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 3));
    TASK_CHECK(obs.sequence[0] == LONG_START_SEQUENCE);
    TASK_CHECK(s->conceal.history_valid);

    oracle_init(&oracle, s, &obs, 2);
    oracle_run(&oracle, 1); /* LONG_START -> EIGHT_SHORT, geometry differs. */
    TASK_CHECK(geometry_short(oracle.last_sequence[0]));
    pcm = NeAACDecConceal(h, &info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT, 5));

    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_normal_long_stop_to_long(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo info;
    void *pcm;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 2));
    TASK_CHECK(obs.sequence[0] == LONG_STOP_SEQUENCE);

    oracle_init(&oracle, s, &obs, 2);
    oracle_run(&oracle, 1); /* LONG_STOP -> ONLY_LONG; both long geometry. */
    TASK_CHECK(oracle.last_sequence[0] == ONLY_LONG_SEQUENCE);
    pcm = NeAACDecConceal(h, &info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT, 4));

    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_normal_short_repeat_spectrum(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    unsigned long k;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 4));
    TASK_CHECK(obs.sequence[0] == EIGHT_SHORT_SEQUENCE);
    oracle_init(&oracle, s, &obs, 2);

    for (k = 1; k <= 4; k++)
    {
        NeAACDecFrameInfo info;
        void *pcm;
        oracle_run(&oracle, k);
        pcm = NeAACDecConceal(h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT,
                                     5 + k));
    }

    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_generated_pns_tns_long_to_short(void)
{
    static const unsigned char asc[2] = {0x11, 0x88}; /* 48 kHz mono */
    unsigned char aus[3][128];
    unsigned char aus_off[2][128];
    unsigned lengths[3];
    unsigned lengths_off[2];
    NeAACDecHandle h, h2;
    NeAACDecStruct *s, *s2;
    Observer obs, obs2;
    Oracle oracle;
    NeAACDecFrameInfo info;
    void *pcm;
    int nonzero = 0, tns_changes = 0;
    unsigned i;
    uint32_t r1, r2;

    memset(&obs, 0, sizeof(obs));
    memset(&obs2, 0, sizeof(obs2));
    lengths[0] = make_sce_au(ONLY_LONG_SEQUENCE, 0, 0, 0, aus[0]);
    lengths[1] = make_sce_au(LONG_START_SEQUENCE, 1, 1, 1, aus[1]);
    lengths[2] = make_sce_au(EIGHT_SHORT_SEQUENCE, 1, 1, 1, aus[2]);
    lengths_off[0] = make_sce_au(ONLY_LONG_SEQUENCE, 0, 0, 0, aus_off[0]);
    lengths_off[1] = make_sce_au(LONG_START_SEQUENCE, 1, 1, 0, aus_off[1]);

    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    h2 = make_handle(FAAD_FMT_FLOAT, asc, &obs2, &s2);
    TASK_CHECK(h && h2);
    TASK_CHECK(decode_generated(h, aus[0], lengths, 2));
    TASK_CHECK(decode_generated(h2, aus_off[0], lengths_off, 2));
    TASK_CHECK(obs.seen[0] && obs2.seen[0]);
    for (i = 0; i < AU_SAMPLES; i++)
    {
        if (obs.spectrum[0][i] != 0.0f)
            nonzero = 1;
        if (obs.spectrum[0][i] != obs2.spectrum[0][i])
            tns_changes = 1;
    }
    TASK_CHECK(nonzero);
    TASK_CHECK(tns_changes); /* TNS-on versus TNS-off moved real values. */
    TASK_CHECK(s->conceal.history_valid);

    oracle_init(&oracle, s, &obs, 1);
    oracle_run(&oracle, 1);
    TASK_CHECK(geometry_short(oracle.last_sequence[0]));
    r1 = s->__r1;
    r2 = s->__r2;
    pcm = NeAACDecConceal(h, &info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT, 3));
    TASK_CHECK(check_rng(s, r1, r2));

    oracle_free(&oracle);
    NeAACDecClose(h);
    NeAACDecClose(h2);
    return 0;
}

/* ---------------------------- continuity ----------------------------- */

static int test_short_loss_past_32(void)
{
    static const unsigned char asc[2] = {0x11, 0x88};
    unsigned char aus[3][128];
    unsigned lengths[3];
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    uint32_t r1, r2;
    unsigned long k;

    memset(&obs, 0, sizeof(obs));
    lengths[0] = make_sce_au(ONLY_LONG_SEQUENCE, 0, 0, 0, aus[0]);
    lengths[1] = make_sce_au(LONG_START_SEQUENCE, 0, 1, 1, aus[1]);
    lengths[2] = make_sce_au(EIGHT_SHORT_SEQUENCE, 0, 1, 1, aus[2]);
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_generated(h, aus[0], lengths, 3));
    TASK_CHECK(obs.sequence[0] == EIGHT_SHORT_SEQUENCE);
    TASK_CHECK(s->conceal.history_valid);

    oracle_init(&oracle, s, &obs, 1);
    r1 = s->__r1;
    r2 = s->__r2;
    for (k = 1; k <= 40; k++)
    {
        NeAACDecFrameInfo info;
        void *pcm;
        oracle_run(&oracle, k);
        pcm = NeAACDecConceal(h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT,
                                     3 + k));
        TASK_CHECK(check_rng(s, r1, r2));
        if (k < 32)
            TASK_CHECK(s->conceal.lost_frames == k);
        else
            TASK_CHECK(s->conceal.lost_frames == 32);
    }

    oracle_free(&oracle);
    NeAACDecClose(h);
    return 0;
}

static int test_mono_44100_single_conceal(void)
{
    static const unsigned char asc[2] = {0x12, 0x08}; /* 44.1 kHz mono */
    unsigned char aus[3][128];
    unsigned lengths[3];
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo info;
    void *pcm;

    memset(&obs, 0, sizeof(obs));
    lengths[0] = make_sce_au(ONLY_LONG_SEQUENCE, 1, 0, 0, aus[0]);
    lengths[1] = make_sce_au(LONG_START_SEQUENCE, 1, 1, 1, aus[1]);
    lengths[2] = make_sce_au(EIGHT_SHORT_SEQUENCE, 1, 1, 0, aus[2]);
    h = make_handle(FAAD_FMT_16BIT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_generated(h, aus[0], lengths, 3));
    TASK_CHECK(get_sample_rate(s->sf_index) == 44100);
    TASK_CHECK(obs.sequence[0] == EIGHT_SHORT_SEQUENCE);

    oracle_init(&oracle, s, &obs, 1);
    oracle_run(&oracle, 1);
    pcm = NeAACDecConceal(h, &info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(info.samplerate == 44100);
    TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_16BIT, 4));

    oracle_free(&oracle);
    NeAACDecClose(h);
    return 0;
}

static int test_next_good_au_recovers(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle a, b;
    NeAACDecStruct *sa, *sb;
    Observer obs;
    Oracle oracle;
    unsigned long k;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    a = make_handle(FAAD_FMT_FLOAT, asc, &obs, &sa);
    b = make_handle(FAAD_FMT_FLOAT, asc, NULL, &sb);
    TASK_CHECK(a && b);
    TASK_CHECK(decode_through(a, &fx, 6));
    TASK_CHECK(decode_through(b, &fx, 6));
    oracle_init(&oracle, sa, &obs, 2);

    for (k = 1; k <= 3; k++)
    {
        NeAACDecFrameInfo info;
        void *pcm = NeAACDecConceal(a, &info);
        TASK_CHECK(pcm != NULL);
        oracle_run(&oracle, k);
        TASK_CHECK(verify_synthesis(sa, &oracle, &info, pcm, FAAD_FMT_FLOAT,
                                     7 + k));
        oracle_apply_to_handle(&oracle, sb);
    }

    {
        NeAACDecFrameInfo ia, ib;
        float *pa, *pb;
        unsigned i;
        double worst = 0.0;

        pa = NeAACDecDecode(a, &ia, fx.data + fx.offset[7], fx.length[7]);
        pb = NeAACDecDecode(b, &ib, fx.data + fx.offset[7], fx.length[7]);
        TASK_CHECK(pa && pb && ia.error == 0 && ib.error == 0);
        TASK_CHECK(ia.samples == ib.samples);
        TASK_CHECK((unsigned long)sa->frame == (unsigned long)sb->frame);
        TASK_CHECK(check_rng(sa, sb->__r1, sb->__r2));
        for (i = 0; i < ia.samples; i++)
        {
            double d = fabs((double)pa[i] - (double)pb[i]);
            if (d > worst)
                worst = d;
            if (!close_ref((double)pa[i], (double)pb[i]))
            {
                fprintf(stderr, "recovery PCM mismatch i%u %f %f\n",
                        i, pa[i], pb[i]);
                TASK_CHECK(0);
            }
        }
        TASK_CHECK(worst >= 0.0);
        TASK_CHECK(sa->conceal.history_valid && sa->conceal.lost_frames == 0);

        /* The recovered good AU established a fresh usable history. */
        {
            NeAACDecFrameInfo ci;
            void *cp = NeAACDecConceal(a, &ci);
            TASK_CHECK(cp != NULL && ci.error == 0 && ci.samples == 2048);
        }
    }

    oracle_free(&oracle);
    NeAACDecClose(a);
    NeAACDecClose(b);
    fixture_free(&fx);
    return 0;
}

static int test_failure_rebuilds_history(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    NeAACDecFrameInfo bad, concealed, good;
    unsigned char truncated[8];
    unsigned long frame_before_failed_conceal;

    memset(&obs, 0, sizeof(obs));
    memset(&bad, 0, sizeof(bad));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 2));

    memcpy(truncated, fx.data + fx.offset[3], 4);
    NeAACDecDecode(h, &bad, truncated, 4);
    TASK_CHECK(bad.error != 0);
    TASK_CHECK(!s->conceal.history_valid);

    frame_before_failed_conceal = s->frame;
    TASK_CHECK(NeAACDecConceal(h, &concealed) == NULL);
    TASK_CHECK(concealed.error != 0 && concealed.samples == 0);
    TASK_CHECK((unsigned long)s->frame == frame_before_failed_conceal);

    NeAACDecDecode(h, &good, fx.data + fx.offset[3], fx.length[3]);
    TASK_CHECK(good.error == 0);
    TASK_CHECK(s->conceal.history_valid);
    {
        NeAACDecFrameInfo ci;
        TASK_CHECK(NeAACDecConceal(h, &ci) != NULL);
        TASK_CHECK(ci.error == 0 && ci.samples == 2048);
    }

    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_reinit_invalidates_and_rebuilds(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    NeAACDecFrameInfo info;
    unsigned long rate;
    unsigned char channels;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 2));
    TASK_CHECK(s->conceal.history_valid);

    TASK_CHECK(NeAACDecInit2(h, (unsigned char*)asc, 2, &rate, &channels) == 0);
    TASK_CHECK(s->conceal.raw_init2 && !s->conceal.history_valid);
    TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    NeAACDecDecode(h, &info, fx.data + fx.offset[3], fx.length[3]);
    TASK_CHECK(info.error == 0 && s->conceal.history_valid);
    TASK_CHECK(NeAACDecConceal(h, &info) != NULL);
    TASK_CHECK(info.error == 0 && info.samples == 2048);

    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_seek_invalidates_and_rebuilds(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    NeAACDecFrameInfo ci, good;
    unsigned long frame_before;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 6));
    TASK_CHECK(s->conceal.history_valid);

    NeAACDecPostSeekReset(h, -1);
    TASK_CHECK(!s->conceal.history_valid);
    frame_before = s->frame;
    TASK_CHECK(NeAACDecConceal(h, &ci) == NULL);
    TASK_CHECK(ci.error != 0 && ci.samples == 0);
    TASK_CHECK((unsigned long)s->frame == frame_before);

    NeAACDecDecode(h, &good, fx.data + fx.offset[7], fx.length[7]);
    TASK_CHECK(good.error == 0 && s->conceal.history_valid);
    TASK_CHECK(NeAACDecConceal(h, &ci) != NULL);
    TASK_CHECK(ci.error == 0 && ci.samples == 2048);

    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

/* ------------------------------ buffers ------------------------------- */

static int test_uninitialized_and_invalid_arguments(void)
{
    NeAACDecFrameInfo info;
    NeAACDecHandle h = NeAACDecOpen();
    static const unsigned char asc[2] = {0x11, 0x90};
    unsigned long rate;
    unsigned char channels;
    void *buffer_value = NULL;

    memset(&info, 0, sizeof(info));
    TASK_CHECK(NeAACDecConceal(NULL, &info) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);
    TASK_CHECK(NeAACDecGetErrorMessage(info.error) != NULL);

    memset(&info, 0, sizeof(info));
    TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);
    TASK_CHECK(NeAACDecGetErrorMessage(info.error) != NULL);
    TASK_CHECK(NeAACDecConceal(h, NULL) == NULL);

    TASK_CHECK(NeAACDecInit2(h, (unsigned char*)asc, 2, &rate, &channels) == 0);
    memset(&info, 0, sizeof(info));
    TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    TASK_CHECK(NeAACDecConceal2(h, &info, NULL, 4096) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    memset(&info, 0, sizeof(info));
    TASK_CHECK(NeAACDecConceal2(h, &info, &buffer_value, 4096) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);

    NeAACDecClose(h);
    return 0;
}

static int buffer_case(unsigned char format, int use_caller_buffer)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo info;
    unsigned long required = AU_SAMPLES * 2 * (format == FAAD_FMT_FLOAT ? 4 : 2);
    void *pcm;
    unsigned char *storage = NULL;
    void *caller;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(format, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 6));
    oracle_init(&oracle, s, &obs, 2);
    oracle_run(&oracle, 1);

    if (use_caller_buffer)
    {
        storage = (unsigned char*)malloc(required);
        TASK_CHECK(storage != NULL);
        caller = storage;
        pcm = NeAACDecConceal2(h, &info, &caller, required);
        TASK_CHECK(pcm == storage);
    }
    else
    {
        pcm = NeAACDecConceal(h, &info);
        TASK_CHECK(pcm == s->sample_buffer);
    }
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, format, 8));

    free(storage);
    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_float_internal_buffer(void)
{
    return buffer_case(FAAD_FMT_FLOAT, 0);
}

static int test_float_caller_buffer(void)
{
    return buffer_case(FAAD_FMT_FLOAT, 1);
}

static int test_16bit_internal_buffer(void)
{
    return buffer_case(FAAD_FMT_16BIT, 0);
}

static int test_16bit_caller_buffer(void)
{
    return buffer_case(FAAD_FMT_16BIT, 1);
}

static int test_insufficient_buffer_is_atomic(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    Oracle oracle;
    NeAACDecFrameInfo info;
    conceal_state state_snapshot;
    unsigned long required = 2048 * (unsigned long)sizeof(float);
    unsigned char *buffer, *backup;
    void *caller;
    uint32_t r1, r2;
    real_t overlap_snapshot[2][AU_SAMPLES];
    uint8_t shape_snapshot[2];

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 6));
    oracle_init(&oracle, s, &obs, 2);
    oracle_run(&oracle, 1);

    buffer = (unsigned char*)malloc(required);
    backup = (unsigned char*)malloc(required);
    TASK_CHECK(buffer && backup);
    memset(buffer, 0x5a, required);
    memcpy(backup, buffer, required);
    caller = buffer;
    state_snapshot = s->conceal;
    memcpy(overlap_snapshot[0], s->fb_intermed[0], sizeof(overlap_snapshot[0]));
    memcpy(overlap_snapshot[1], s->fb_intermed[1], sizeof(overlap_snapshot[1]));
    shape_snapshot[0] = s->window_shape_prev[0];
    shape_snapshot[1] = s->window_shape_prev[1];
    r1 = s->__r1;
    r2 = s->__r2;

    TASK_CHECK(NeAACDecConceal2(h, &info, &caller, required - 1) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    TASK_CHECK(memcmp(buffer, backup, required) == 0);
    TASK_CHECK(memcmp(&s->conceal, &state_snapshot, sizeof(state_snapshot)) == 0);
    TASK_CHECK(memcmp(s->fb_intermed[0], overlap_snapshot[0],
                      sizeof(overlap_snapshot[0])) == 0);
    TASK_CHECK(memcmp(s->fb_intermed[1], overlap_snapshot[1],
                      sizeof(overlap_snapshot[1])) == 0);
    TASK_CHECK(s->window_shape_prev[0] == shape_snapshot[0] &&
               s->window_shape_prev[1] == shape_snapshot[1]);
    TASK_CHECK(check_rng(s, r1, r2));
    TASK_CHECK((unsigned long)s->frame == 7);

    /* Retrying with enough capacity is exactly the first missing AU. */
    {
        void *pcm = NeAACDecConceal2(h, &info, &caller, required);
        TASK_CHECK(pcm == buffer);
        TASK_CHECK(verify_synthesis(s, &oracle, &info, pcm, FAAD_FMT_FLOAT, 8));
        TASK_CHECK(s->conceal.lost_frames == 1);
    }

    free(buffer);
    free(backup);
    oracle_free(&oracle);
    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_adts_is_out_of_scope(void)
{
    Fixture fx;
    NeAACDecHandle h;
    NeAACDecStruct *s;
    NeAACDecConfigurationPtr cfg;
    NeAACDecFrameInfo info, conceal_info;
    unsigned long rate = 0;
    unsigned char channels = 0;

    TASK_CHECK(fixture_load(&fx));
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->dontUpSampleImplicitSBR = 1;
    TASK_CHECK(NeAACDecSetConfiguration(h, cfg));
    TASK_CHECK(NeAACDecInit(h, fx.data, fx.size, &rate, &channels) >= 0);
    s = (NeAACDecStruct*)h;
    TASK_CHECK(s->adts_header_present);

    NeAACDecDecode(h, &info, fx.data, 7 + fx.length[0]);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(!s->conceal.history_valid);
    TASK_CHECK(NeAACDecConceal(h, &conceal_info) == NULL);
    TASK_CHECK(conceal_info.error != 0 && conceal_info.samples == 0);

    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

static int test_unsupported_output_format_rejected(void)
{
    Fixture fx;
    static const unsigned char asc[2] = {0x11, 0x90};
    NeAACDecHandle h;
    NeAACDecStruct *s;
    Observer obs;
    NeAACDecFrameInfo info;
    unsigned frame;

    memset(&obs, 0, sizeof(obs));
    TASK_CHECK(fixture_load(&fx));
    h = make_handle(FAAD_FMT_FLOAT, asc, &obs, &s);
    TASK_CHECK(h != NULL);
    TASK_CHECK(decode_through(h, &fx, 6));
    s->config.outputFormat = FAAD_FMT_24BIT;
    frame = s->frame;
    TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);
    TASK_CHECK(s->frame == frame);
    s->config.outputFormat = FAAD_FMT_FLOAT;
    TASK_CHECK(NeAACDecConceal(h, &info) != NULL);
    TASK_CHECK(info.error == 0);

    NeAACDecClose(h);
    fixture_free(&fx);
    return 0;
}

const struct task_case task_cases[] = {
    {"spectrum", "normal_stereo_reference", test_normal_stereo_spectrum},
    {"spectrum", "first_good_au_priming", test_first_good_au_still_suppressed},
    {"spectrum", "normal_long_start_to_short_zero", test_normal_long_start_to_short},
    {"spectrum", "normal_long_stop_to_long", test_normal_long_stop_to_long},
    {"spectrum", "normal_short_repeat", test_normal_short_repeat_spectrum},
    {"spectrum", "generated_pns_tns_long_to_short", test_generated_pns_tns_long_to_short},

    {"continuity", "short_loss_through_40", test_short_loss_past_32},
    {"continuity", "mono_44100_pcm16", test_mono_44100_single_conceal},
    {"continuity", "next_good_au_recovery", test_next_good_au_recovers},
    {"continuity", "failure_rebuilds_history", test_failure_rebuilds_history},
    {"continuity", "reinit_rebuilds_history", test_reinit_invalidates_and_rebuilds},
    {"continuity", "seek_rebuilds_history", test_seek_invalidates_and_rebuilds},

    {"buffers", "uninitialized_and_invalid_arguments", test_uninitialized_and_invalid_arguments},
    {"buffers", "float_internal", test_float_internal_buffer},
    {"buffers", "float_caller", test_float_caller_buffer},
    {"buffers", "pcm16_internal", test_16bit_internal_buffer},
    {"buffers", "pcm16_caller", test_16bit_caller_buffer},
    {"buffers", "insufficient_is_atomic", test_insufficient_buffer_is_atomic},
    {"buffers", "adts_is_out_of_scope", test_adts_is_out_of_scope},
    {"buffers", "unsupported_output_format", test_unsupported_output_format_rejected},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
