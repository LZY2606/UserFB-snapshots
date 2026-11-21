/*
 * Concealment tests: NeAACDecConceal / NeAACDecConceal2.
 *
 * Groups:
 *   spectrum   - the concealed spectrum is the cached good-AU spectrum times
 *                0.75^k (zero once k >= 32), only reused when the synthesis
 *                geometry matches.
 *   continuity - the real filter bank state (overlap, pre window shape, frame
 *                counter) advances like an independent reference that keeps
 *                its own spectrum/overlap and runs the existing filter bank.
 *   buffers    - NeAACDecConceal2 buffer handling, invalid/out-of-scope calls,
 *                invalidation after a failed AU / PostSeekReset, and the
 *                float/16-bit output paths.
 *
 * Reference expectations are snapshotted from a single good AU and then
 * advanced independently; concealed decoder state is never fed back into the
 * model.
 */
#include "../task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"
#include "../task_support/conceal_fixture_impl.h"

extern uint8_t faad_conceal_test_peek(NeAACDecHandle hDecoder, uint8_t channel,
                                      const float **spectrum,
                                      uint8_t *window_sequence,
                                      uint8_t *window_shape, uint32_t *lost,
                                      uint8_t *next_sequence,
                                      const float **overlap,
                                      uint8_t *window_shape_prev,
                                      uint32_t *frame_counter,
                                      uint32_t *rng1, uint32_t *rng2);
extern void *task_test_fb_create(uint16_t frame_len);
extern void task_test_fb_run(void *fb_state, uint8_t window_sequence,
                             uint8_t window_shape, uint8_t window_shape_prev,
                             const float *spectrum, float *time_out,
                             float *overlap, uint16_t frame_len);
extern void task_test_fb_end(void *fb_state);

#define N 1024
#define TOL(base) (2e-5 * (1.0 + fabs((double)(base))))

#define SEQ_ONLY_LONG 0
#define SEQ_LONG_START 1
#define SEQ_EIGHT_SHORT 2
#define SEQ_LONG_STOP 3

static const unsigned char asc_mono[2] = {0x11, 0x88};
static const unsigned char asc_stereo[2] = {0x11, 0x90};

static void fill_desc(fixture_au_desc *d, int channels, int seq, int common,
                      int gain, int value)
{
    memset(d, 0, sizeof(*d));
    d->channels = channels;
    d->window_sequence = seq;
    d->window_shape = 0;
    d->common_window = common;
    d->ms_mask_present = 0;
    d->tns = 0;
    d->noise_start_sfb = -1;
    d->intensity_start_sfb = -1;
    d->noise_energy = 0;
    d->gain = gain;
    d->value = value;
}

static unsigned char *make_au(const fixture_au_desc *d, size_t *len)
{
    fixture_bw bw;

    fixture_bw_init(&bw, 16384);
    if (fixture_make_au(&bw, d) != 0)
    {
        fixture_bw_free(&bw);
        return NULL;
    }
    *len = fixture_bw_bytes(&bw);
    return bw.data;
}

typedef struct
{
    NeAACDecHandle h;
    unsigned long rate;
    unsigned char channels;
} dec;

static int dec_open(dec *d, const unsigned char *asc, unsigned char fmt)
{
    NeAACDecConfigurationPtr c;

    memset(d, 0, sizeof(*d));
    d->h = NeAACDecOpen();
    if (!d->h)
        return -1;
    c = NeAACDecGetCurrentConfiguration(d->h);
    if (!c)
        return -1;
    c->outputFormat = fmt;
    c->defObjectType = LC;
    c->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(d->h, c))
        return -1;
    if (NeAACDecInit2(d->h, (unsigned char*)asc, 2, &d->rate, &d->channels) != 0)
        return -1;
    return 0;
}

static int dec_au(dec *d, const fixture_au_desc *desc, NeAACDecFrameInfo *info_out)
{
    size_t len = 0;
    unsigned char *buf = make_au(desc, &len);
    NeAACDecFrameInfo info;
    void *pcm;

    if (!buf)
        return -1;
    pcm = NeAACDecDecode(d->h, &info, buf, (unsigned long)len);
    free(buf);
    if (info.error != 0 || pcm == NULL)
        return -1;
    if (info.bytesconsumed != len)
        return -1;
    if (info_out)
        *info_out = info;
    return 0;
}

/* The reference keeps the good AU's spectrum/overlap and an independent
   filter bank instance sharing no state with the decoder under test. */
typedef struct
{
    int channels;
    int valid;
    uint8_t seq[2];
    uint8_t shape[2];
    float spec[2][N];
    float overlap[2][N];
    uint8_t shape_prev[2];
    uint8_t last_seq[2];
    uint32_t k;
    void *fb;
    float time_out[2][N];
} reference;

static uint8_t ref_next_sequence(uint8_t previous)
{
    if (previous == SEQ_LONG_START || previous == SEQ_EIGHT_SHORT)
        return SEQ_EIGHT_SHORT;
    return SEQ_ONLY_LONG;
}

static int ref_geometry_matches(uint8_t cached, uint8_t synthesis)
{
    return (cached == SEQ_EIGHT_SHORT) == (synthesis == SEQ_EIGHT_SHORT);
}

static int reference_capture(dec *d, reference *ref)
{
    uint8_t ch;
    const float *spec = NULL, *overlap = NULL;
    uint8_t seq = 0, shape = 0, next_seq = 0, shape_prev = 0;
    uint32_t lost = 0, frame = 0;

    memset(ref, 0, sizeof(*ref));
    ref->channels = d->channels;
    for (ch = 0; ch < d->channels; ch++)
    {
        if (!faad_conceal_test_peek(d->h, ch, &spec, &seq, &shape, &lost,
                                    &next_seq, &overlap, &shape_prev, &frame, NULL, NULL))
            return -1;
        memcpy(ref->spec[ch], spec, N * sizeof(float));
        memcpy(ref->overlap[ch], overlap, N * sizeof(float));
        ref->seq[ch] = seq;
        ref->shape[ch] = shape;
        ref->shape_prev[ch] = shape_prev;
        ref->last_seq[ch] = seq;
    }
    ref->k = 0;
    ref->fb = task_test_fb_create(N);
    if (!ref->fb)
        return -1;
    ref->valid = 1;
    return 0;
}

static void reference_end(reference *ref)
{
    task_test_fb_end(ref->fb);
    ref->fb = NULL;
}

/* Advance the reference by one missing AU.  `out` receives interleaved PCM of
   the same form output_to_PCM produces (float, FLOAT_SCALE applied). */
static int reference_advance(reference *ref, float *out, uint8_t *seq_used)
{
    int ch, i;

    ref->k++;
    for (ch = 0; ch < ref->channels; ch++)
    {
        uint8_t synthesis = ref_next_sequence(ref->last_seq[ch]);
        float spec[N];
        float scale;

        if (ref->k >= 32 || !ref_geometry_matches(ref->seq[ch], synthesis))
            memset(spec, 0, sizeof(spec));
        else
        {
            scale = (float)pow(0.75, (double)ref->k);
            for (i = 0; i < N; i++)
                spec[i] = ref->spec[ch][i] * scale;
        }

        task_test_fb_run(ref->fb, synthesis, ref->shape[ch],
                         ref->shape_prev[ch], spec, ref->time_out[ch],
                         ref->overlap[ch], N);

        ref->shape_prev[ch] = ref->shape[ch];
        ref->last_seq[ch] = synthesis;
        if (seq_used)
            seq_used[ch] = synthesis;
    }

    /* mirror output_to_PCM's float conversion and interleaving */
    for (i = 0; i < N; i++)
        for (ch = 0; ch < ref->channels; ch++)
            out[i * ref->channels + ch] = ref->time_out[ch][i] * (1.0f / (1 << 15));

    return 0;
}

static int compare_float(const float *got, const float *want, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
    {
        double tol = TOL(want[i]);
        if (fabs((double)got[i] - (double)want[i]) > tol)
            return -1;
    }
    return 0;
}

static int compare_overlap(dec *d, const reference *ref)
{
    uint8_t ch;
    const float *overlap = NULL;

    for (ch = 0; ch < ref->channels; ch++)
    {
        int i;
        if (!faad_conceal_test_peek(d->h, ch, &overlap, NULL, NULL, NULL, NULL,
                                    &overlap, NULL, NULL, NULL, NULL))
            return -1;
        for (i = 0; i < N; i++)
        {
            double want = ref->overlap[ch][i];
            if (fabs((double)overlap[i] - want) > TOL(want))
                return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* spectrum group                                                            */
/* ------------------------------------------------------------------------- */

/* k-th missing AU: cache * 0.75^k, cache untouched, geometry rules */
static int case_decay(void)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    const float *spec = NULL;
    uint8_t seq, shape, next_seq, shape_prev;
    uint32_t lost, frame;
    float cached[N];
    int k, i;

    if (dec_open(&a, asc_mono, FAAD_FMT_FLOAT) != 0)
        return 1;
    fill_desc(&d, 1, SEQ_ONLY_LONG, 0, 100, 1);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&a, &d, &info) == 0);
    TASK_CHECK(info.samples == 1024);

    TASK_CHECK(faad_conceal_test_peek(a.h, 0, &spec, &seq, &shape, &lost,
                                      &next_seq, NULL, &shape_prev, &frame, NULL, NULL) == 1);
    memcpy(cached, spec, sizeof(cached));
    TASK_CHECK(lost == 0);
    TASK_CHECK(seq == SEQ_ONLY_LONG);

    for (k = 1; k <= 5; k++)
    {
        void *pcm = NeAACDecConceal(a.h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(info.bytesconsumed == 0);
        TASK_CHECK(info.samples == 1024);
        TASK_CHECK(info.channels == 1);
        TASK_CHECK(info.samplerate == 48000);
        TASK_CHECK(info.header_type == 0);

        TASK_CHECK(faad_conceal_test_peek(a.h, 0, &spec, &seq, &shape, &lost,
                                          &next_seq, NULL, &shape_prev, &frame, NULL, NULL) == 1);
        TASK_CHECK(lost == (uint32_t)k);
        /* the cache itself is never attenuated */
        for (i = 0; i < N; i++)
            TASK_CHECK(spec[i] == cached[i]);
        TASK_CHECK(seq == SEQ_ONLY_LONG);
        TASK_CHECK(next_seq == SEQ_ONLY_LONG);
        (void)pcm;
    }

    NeAACDecClose(a.h);
    return 0;
}

/* k >= 32 uses a zero spectrum; the output eventually becomes exactly 0 */
static int case_silent_tail(void)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    int i, j;

    if (dec_open(&a, asc_mono, FAAD_FMT_FLOAT) != 0)
        return 1;
    fill_desc(&d, 1, SEQ_ONLY_LONG, 0, 100, 1);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&a, &d, &info) == 0);

    for (i = 1; i <= 40; i++)
    {
        const float *pcm = NeAACDecConceal(a.h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(info.samples == 1024);
        if (i >= 40)
        {
            for (j = 0; j < N; j++)
                TASK_CHECK(pcm[j] == 0.0f);
        }
    }

    NeAACDecClose(a.h);
    return 0;
}

/* PNS + TNS + M/S stereo: concealment must not consume random numbers and
   must not re-run TNS/DRC.  The RNG registers stay bit-identical across
   concealed access units. */
static int case_pns_rng(void)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    uint32_t r1_before = 0, r2_before = 0, r1_after = 0, r2_after = 0;

    if (dec_open(&a, asc_stereo, FAAD_FMT_FLOAT) != 0)
        return 1;

    fill_desc(&d, 2, SEQ_ONLY_LONG, 1, 100, 1);
    d.ms_mask_present = 2;
    d.tns = 2;
    d.noise_start_sfb = 8;
    d.noise_energy = -10;

    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);

    TASK_CHECK(faad_conceal_test_peek(a.h, 0, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, &r1_before,
                                      &r2_before) == 1);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    TASK_CHECK(info.samples == 2048);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    TASK_CHECK(faad_conceal_test_peek(a.h, 0, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, &r1_after,
                                      &r2_after) == 1);
    TASK_CHECK(r1_before == r1_after);
    TASK_CHECK(r2_before == r2_after);

    NeAACDecClose(a.h);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* continuity group                                                          */
/* ------------------------------------------------------------------------- */

static int run_continuity(int channels, int first_seq, int good_seq, int count,
                          int long_to_short)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    reference ref;
    float want[N * 2];
    uint8_t seq_used[2];
    int n;

    if (dec_open(&a, channels == 1 ? asc_mono : asc_stereo, FAAD_FMT_FLOAT) != 0)
        return 1;

    /* one priming AU, then a full good AU that establishes the cache */
    fill_desc(&d, channels, first_seq, channels == 2, 100, 1);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    fill_desc(&d, channels, good_seq, channels == 2, 100, 1);
    TASK_CHECK(dec_au(&a, &d, &info) == 0);
    TASK_CHECK(info.samples == (unsigned long)(N * channels));

    if (long_to_short)
    {
        /* drive a real LONG_START AU so the cache says "long start", then
           conceal: the synthesis must be a short window with a zero spectrum */
        fixture_au_desc pre;
        fill_desc(&pre, channels, SEQ_LONG_START, channels == 2, 100, 1);
        TASK_CHECK(dec_au(&a, &pre, &info) == 0);
    }

    TASK_CHECK(reference_capture(&a, &ref) == 0);

    for (n = 0; n < count; n++)
    {
        const float *pcm = NeAACDecConceal(a.h, &info);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(info.samples == (unsigned long)(N * channels));
        TASK_CHECK(reference_advance(&ref, want, seq_used) == 0);
        TASK_CHECK(compare_float(pcm, want, (size_t)info.samples) == 0);
        TASK_CHECK(compare_overlap(&a, &ref) == 0);
        /* the decoder's next synthesis sequence and pre window shape must
           match the reference exactly */
        {
            int ch;
            for (ch = 0; ch < channels; ch++)
            {
                uint8_t next_seq = 0, shape_prev = 0;
                TASK_CHECK(faad_conceal_test_peek(a.h, (uint8_t)ch, NULL, NULL,
                                                  NULL, NULL, &next_seq, NULL,
                                                  &shape_prev, NULL, NULL,
                                                  NULL) == 1);
                TASK_CHECK(next_seq == ref_next_sequence(ref.last_seq[ch]));
                TASK_CHECK(shape_prev == ref.shape[ch]);
            }
        }
    }

    /* a following good AU must continue from this state and reset the count */
    fill_desc(&d, channels, good_seq, channels == 2, 100, 1);
    TASK_CHECK(dec_au(&a, &d, &info) == 0);
    {
        uint32_t lost = 1;
        TASK_CHECK(faad_conceal_test_peek(a.h, 0, NULL, NULL, NULL, &lost,
                                          NULL, NULL, NULL, NULL, NULL, NULL) == 1);
        TASK_CHECK(lost == 0);
    }

    reference_end(&ref);
    NeAACDecClose(a.h);
    return 0;
}

static int case_long_only(void)   { return run_continuity(1, SEQ_ONLY_LONG, SEQ_ONLY_LONG, 4, 0); }
static int case_stereo(void)      { return run_continuity(2, SEQ_ONLY_LONG, SEQ_ONLY_LONG, 4, 0); }
static int case_short_run(void)   { return run_continuity(1, SEQ_EIGHT_SHORT, SEQ_EIGHT_SHORT, 6, 0); }
static int case_over32(void)      { return run_continuity(1, SEQ_ONLY_LONG, SEQ_ONLY_LONG, 40, 0); }
static int case_long_to_short(void) { return run_continuity(1, SEQ_ONLY_LONG, SEQ_ONLY_LONG, 2, 1); }

/* ------------------------------------------------------------------------- */
/* buffers group                                                             */
/* ------------------------------------------------------------------------- */

static void prepare_good(dec *a, int channels)
{
    fixture_au_desc d;
    fill_desc(&d, channels, SEQ_ONLY_LONG, channels == 2, 100, 1);
    (void)dec_au(a, &d, NULL);
    (void)dec_au(a, &d, NULL);
}

/* NeAACDecConceal2: too small / null / non-null pointer to caller buffer */
static int case_decode2_buffer(void)
{
    dec a, ref;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    unsigned char small[64];
    unsigned char small_before[sizeof(small)];
    void *ptr;
    float *buf;
    float want[N];
    reference r;
    uint8_t seq_used[1];

    if (dec_open(&a, asc_mono, FAAD_FMT_FLOAT) != 0)
        return 1;
    prepare_good(&a, 1);
    if (dec_open(&ref, asc_mono, FAAD_FMT_FLOAT) != 0)
        return 1;
    prepare_good(&ref, 1);
    TASK_CHECK(reference_capture(&ref, &r) == 0);
    TASK_CHECK(reference_advance(&r, want, seq_used) == 0);

    /* capacity in bytes, must be the whole frame */
    TASK_CHECK(NeAACDecConceal2(a.h, &info, NULL, 0) == NULL);
    TASK_CHECK(info.error == 27);
    TASK_CHECK(info.samples == 0);

    memset(small, 0x5A, sizeof(small));
    memcpy(small_before, small, sizeof(small));
    ptr = small;
    TASK_CHECK(NeAACDecConceal2(a.h, &info, &ptr, sizeof(small)) == NULL);
    TASK_CHECK(info.error == 27);
    TASK_CHECK(info.samples == 0);
    TASK_CHECK(memcmp(small, small_before, sizeof(small)) == 0);

    buf = (float*)malloc(N * sizeof(float));
    TASK_CHECK(buf != NULL);
    memset(buf, 0xA5, N * sizeof(float));
    ptr = buf;
    /* one byte short of a whole frame */
    TASK_CHECK(NeAACDecConceal2(a.h, &info, &ptr, N * sizeof(float) - 1) == NULL);
    TASK_CHECK(info.error == 27);
    TASK_CHECK(info.samples == 0);
    {
        int i, unchanged = 1;
        unsigned char *b = (unsigned char*)buf;
        for (i = 0; i < (int)(N * sizeof(float)); i++)
            if (b[i] != 0xA5)
                unchanged = 0;
        TASK_CHECK(unchanged);
    }

    /* a rejected call must not have advanced the frame counter or the cache */
    {
        uint32_t lost = 99, frame = 0;
        TASK_CHECK(faad_conceal_test_peek(a.h, 0, NULL, NULL, NULL, &lost,
                                          NULL, NULL, NULL, &frame, NULL, NULL) == 1);
        TASK_CHECK(lost == 0);
    }

    /* giving enough capacity must behave like a successful call */
    ptr = buf;
    TASK_CHECK(NeAACDecConceal2(a.h, &info, &ptr, N * sizeof(float)) == buf);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(info.bytesconsumed == 0);
    TASK_CHECK(compare_float(buf, want, N) == 0);
    TASK_CHECK(compare_overlap(&a, &r) == 0);

    reference_end(&r);
    free(buf);
    NeAACDecClose(a.h);
    NeAACDecClose(ref.h);
    (void)d;
    return 0;
}

/* invalid calls: before a good AU, after a failed AU and after a seek */
static int case_invalidation(void)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;

    if (dec_open(&a, asc_mono, FAAD_FMT_FLOAT) != 0)
        return 1;

    /* no good AU yet */
    TASK_CHECK(NeAACDecConceal(a.h, &info) == NULL);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);

    fill_desc(&d, 1, SEQ_ONLY_LONG, 0, 100, 1);
    /* the first good AU suppresses its samples but still counts as good */
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    TASK_CHECK(info.samples == 1024);

    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    TASK_CHECK(info.samples == 1024);

    /* a failed decode invalidates the cache */
    {
        unsigned char junk[16];
        memset(junk, 0xFF, sizeof(junk));
        (void)NeAACDecDecode(a.h, &info, junk, sizeof(junk));
        TASK_CHECK(info.error != 0);
    }
    TASK_CHECK(NeAACDecConceal(a.h, &info) == NULL);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);

    /* rebuild, then a seek clears it again */
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    NeAACDecPostSeekReset(a.h, -1);
    TASK_CHECK(NeAACDecConceal(a.h, &info) == NULL);
    TASK_CHECK(info.samples == 0);

    /* the next good AU re-establishes history */
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(NeAACDecConceal(a.h, &info) != NULL);
    TASK_CHECK(info.samples == 1024);

    NeAACDecClose(a.h);
    return 0;
}

/* out-of-scope formats must not be concealed automatically: ADTS input */
static int case_out_of_scope(void)
{
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecFrameInfo info;
    unsigned long rate;
    unsigned char ch;

    TASK_CHECK(h != NULL);
    {
        NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(h);
        c->outputFormat = FAAD_FMT_FLOAT;
        TASK_CHECK(NeAACDecSetConfiguration(h, c) != 0);
    }
    TASK_CHECK(NeAACDecInit2(h, (unsigned char*)asc_mono, 2, &rate, &ch) == 0);
    /* no good AU yet: must refuse and leave samples at 0 */
    TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
    TASK_CHECK(info.samples == 0);
    TASK_CHECK(info.error != 0);
    NeAACDecClose(h);

    /* an ADTS-init decoder keeps decoding normally but never conceals */
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    {
        FILE *f = fopen("task_support/normal.aac", "rb");
        unsigned char data[8192];
        size_t got;
        unsigned long aref = 0;
        unsigned char chans = 0;

        TASK_CHECK(f != NULL);
        if (!f)
        {
            NeAACDecClose(h);
            return 1;
        }
        got = fread(data, 1, sizeof(data), f);
        fclose(f);
        TASK_CHECK(got > 8);
        TASK_CHECK(NeAACDecInit(h, data, (unsigned long)got, &aref, &chans) == 0);
        /* decode one complete ADTS frame from the buffer (skip the header) */
        {
            unsigned len = ((unsigned)(data[3] & 3) << 11) |
                           ((unsigned)data[4] << 3) | (data[5] >> 5);
            const float *pcm = NeAACDecDecode(h, &info, data, len);
            TASK_CHECK(info.error == 0);
            TASK_CHECK(pcm != NULL);
            /* concealment is not available for ADTS: refuses without touching
               the decoded stream state */
            TASK_CHECK(NeAACDecConceal(h, &info) == NULL);
            TASK_CHECK(info.samples == 0);
            TASK_CHECK(info.error != 0);
        }
    }
    NeAACDecClose(h);
    return 0;
}

/* NeAACDecConceal and NeAACDecConceal2 must advance identical state */
static int case_conceal_vs_conceal2(void)
{
    dec a, b;
    fixture_au_desc d;
    NeAACDecFrameInfo ia, ib;
    float *buf;
    void *slot;
    int round;

    if (dec_open(&a, asc_stereo, FAAD_FMT_FLOAT) != 0)
        return 1;
    if (dec_open(&b, asc_stereo, FAAD_FMT_FLOAT) != 0)
        return 1;
    fill_desc(&d, 2, SEQ_ONLY_LONG, 1, 200, 1);
    d.tns = 2;
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&b, &d, NULL) == 0);
    TASK_CHECK(dec_au(&b, &d, NULL) == 0);

    buf = (float*)malloc(2048 * sizeof(float));
    TASK_CHECK(buf != NULL);
    for (round = 0; round < 3; round++)
    {
        const float *pa = NeAACDecConceal(a.h, &ia);
        void *pb;
        slot = buf;
        pb = NeAACDecConceal2(b.h, &ib, &slot, 2048 * sizeof(float));
        TASK_CHECK(pa != NULL && pb == buf);
        TASK_CHECK(ia.error == 0 && ib.error == 0);
        TASK_CHECK(ia.samples == ib.samples && ia.samples == 2048);
        TASK_CHECK(memcmp(pa, pb, ia.samples * sizeof(float)) == 0);
        /* identical overlap and bookkeeping after either entrance */
        {
            int ch;
            for (ch = 0; ch < 2; ch++)
            {
                const float *oa = NULL, *ob = NULL;
                uint8_t na = 1, nb = 1, sa = 1, sb = 1;
                uint32_t la = 1, lb = 1, fa = 0, fb = 0;
                int i;
                TASK_CHECK(faad_conceal_test_peek(a.h, (uint8_t)ch, NULL, NULL,
                                                  NULL, &la, &na, NULL, &sa,
                                                  &fa, NULL, NULL) == 1);
                TASK_CHECK(faad_conceal_test_peek(b.h, (uint8_t)ch, NULL, NULL,
                                                  NULL, &lb, &nb, NULL, &sb,
                                                  &fb, NULL, NULL) == 1);
                TASK_CHECK(la == lb && na == nb && sa == sb && fa == fb);
                TASK_CHECK(faad_conceal_test_peek(a.h, (uint8_t)ch, NULL, NULL,
                                                  NULL, NULL, NULL, &oa, NULL,
                                                  NULL, NULL, NULL) == 1);
                TASK_CHECK(faad_conceal_test_peek(b.h, (uint8_t)ch, NULL, NULL,
                                                  NULL, NULL, NULL, &ob, NULL,
                                                  NULL, NULL, NULL) == 1);
                for (i = 0; i < N; i++)
                    TASK_CHECK(oa[i] == ob[i]);
            }
        }
    }
    free(buf);
    NeAACDecClose(a.h);
    NeAACDecClose(b.h);
    return 0;
}


static int case_16bit(void)
{
    dec a;
    fixture_au_desc d;
    NeAACDecFrameInfo info;
    int16_t *pcm;
    int i, nonzero = 0;

    if (dec_open(&a, asc_stereo, FAAD_FMT_16BIT) != 0)
        return 1;
    fill_desc(&d, 2, SEQ_ONLY_LONG, 1, 200, 1);
    TASK_CHECK(dec_au(&a, &d, NULL) == 0);
    TASK_CHECK(dec_au(&a, &d, &info) == 0);
    TASK_CHECK(info.samples == 2048);

    pcm = (int16_t*)NeAACDecConceal(a.h, &info);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == 0);
    TASK_CHECK(info.samples == 2048);
    TASK_CHECK(info.channels == 2);
    for (i = 0; i < 2048; i++)
        if (pcm[i] != 0)
            nonzero = 1;
    TASK_CHECK(nonzero);

    /* a second call also yields 1024 samples per channel and keeps counting */
    pcm = (int16_t*)NeAACDecConceal(a.h, &info);
    TASK_CHECK(pcm != NULL && info.samples == 2048);

    /* 16-bit through the caller-supplied buffer as well */
    {
        int16_t out[2048];
        void *slot = out;
        void *ret;
        for (i = 0; i < 2048; i++)
            out[i] = (int16_t)0x1234;
        ret = NeAACDecConceal2(a.h, &info, &slot, sizeof(out));
        TASK_CHECK(ret == out);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(info.samples == 2048);
        TASK_CHECK(!(out[0] == (int16_t)0x1234 && out[2047] == (int16_t)0x1234));
    }

    NeAACDecClose(a.h);
    return 0;
}

const struct task_case task_cases[] = {
    {"spectrum", "decay", case_decay},
    {"spectrum", "silent_tail", case_silent_tail},
    {"spectrum", "pns_rng", case_pns_rng},

    {"continuity", "long_only", case_long_only},
    {"continuity", "stereo", case_stereo},
    {"continuity", "short_run", case_short_run},
    {"continuity", "over32", case_over32},
    {"continuity", "long_to_short", case_long_to_short},

    {"buffers", "conceal2_buffer", case_decode2_buffer},
    {"buffers", "conceal_vs_conceal2", case_conceal_vs_conceal2},
    {"buffers", "invalidation", case_invalidation},
    {"buffers", "out_of_scope", case_out_of_scope},
    {"buffers", "16bit", case_16bit},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
