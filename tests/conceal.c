/* White-box and black-box tests for NeAACDecConceal/NeAACDecConceal2.
 *
 * The white-box access is intentionally read-only as permitted by
 * FAAD_CONCEAL_TEST: tests copy the committed pre-filterbank spectrum and
 * overlap, then run an independent filterbank instance as their oracle.
 */
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#ifndef FAAD_CONCEAL_TEST
#define FAAD_CONCEAL_TEST 1
#endif

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
#include "conceal.h"

#define MAX_TEST_AUS 32
#define AU_CAPACITY 8192

typedef struct {
    unsigned char data[AU_CAPACITY];
    unsigned long len;
} TestAU;

typedef struct {
    int channels;
    int decoder_channel[2];
    fb_info *fb;
    real_t base[2][1024];
    real_t work[2][1024];
    real_t overlap[2][1024];
    real_t time_out[2][1024];
    uint8_t history_sequence[2];
    uint8_t last_sequence[2];
    uint8_t window_shape[2];
    uint8_t previous_shape[2];
    uint32_t start_frame;
    unsigned long sample_rate;
} Oracle;

static int load_normal_aus(TestAU *aus, int max_aus, int *count)
{
    FILE *file = fopen("task_support/normal.aac", "rb");
    unsigned char *file_data = NULL;
    long file_len;
    size_t at = 0;

    *count = 0;
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    file_len = ftell(file);
    if (file_len <= 7) { fclose(file); return 0; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    file_data = malloc((size_t)file_len);
    if (!file_data || fread(file_data, 1, (size_t)file_len, file) != (size_t)file_len) {
        free(file_data);
        fclose(file);
        return 0;
    }
    fclose(file);

    while (at + 7 <= (size_t)file_len && *count < max_aus) {
        unsigned frame_len = ((unsigned)(file_data[at + 3] & 3) << 11) |
                             ((unsigned)file_data[at + 4] << 3) |
                             (file_data[at + 5] >> 5);
        if (file_data[at] != 0xff || (file_data[at + 1] & 0xf0) != 0xf0 ||
            frame_len < 7 || at + frame_len > (size_t)file_len)
            break;
        aus[*count].len = frame_len - 7;
        memcpy(aus[*count].data, file_data + at + 7, aus[*count].len);
        at += frame_len;
        (*count)++;
    }

    free(file_data);
    return *count == 24;
}

static NeAACDecHandle make_handle(unsigned char output_format,
                                  const unsigned char *asc,
                                  unsigned long asc_len)
{
    NeAACDecHandle handle = NeAACDecOpen();
    NeAACDecConfigurationPtr config;
    unsigned long rate = 0;
    unsigned char channels = 0;

    if (!handle)
        return NULL;
    config = NeAACDecGetCurrentConfiguration(handle);
    config->outputFormat = output_format;
    config->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(handle, config)) {
        NeAACDecClose(handle);
        return NULL;
    }
    if (NeAACDecInit2(handle, (unsigned char *)asc, asc_len,
                      &rate, &channels) != 0) {
        NeAACDecClose(handle);
        return NULL;
    }
    return handle;
}

static NeAACDecHandle make_48k_stereo(int format)
{
    static const unsigned char asc[2] = { 0x11, 0x90 };
    return make_handle((unsigned char)format, asc, sizeof(asc));
}

static NeAACDecHandle make_48k_mono(int format)
{
    static const unsigned char asc[2] = { 0x11, 0x88 };
    return make_handle((unsigned char)format, asc, sizeof(asc));
}

static int decode_aus(NeAACDecHandle handle, TestAU *aus, int first, int last)
{
    int i;
    for (i = first; i <= last; i++) {
        NeAACDecFrameInfo info;
        void *pcm = NeAACDecDecode(handle, &info, aus[i].data, aus[i].len);
        if (info.error != 0) {
            fprintf(stderr, "decode AU %d failed error=%u: %s\n", i,
                    info.error, NeAACDecGetErrorMessage(info.error));
            return 0;
        }
        if (i > 0 && (pcm == NULL || info.samples == 0)) {
            fprintf(stderr, "decode AU %d produced no samples\n", i);
            return 0;
        }
    }
    return 1;
}

static int sequence_is_short(uint8_t sequence)
{
    return sequence == EIGHT_SHORT_SEQUENCE;
}

static uint8_t advance_sequence(uint8_t sequence)
{
    if (sequence == LONG_START_SEQUENCE)
        return EIGHT_SHORT_SEQUENCE;
    if (sequence == EIGHT_SHORT_SEQUENCE)
        return EIGHT_SHORT_SEQUENCE;
    if (sequence == LONG_STOP_SEQUENCE)
        return ONLY_LONG_SEQUENCE;
    return ONLY_LONG_SEQUENCE;
}

static void oracle_capture(Oracle *oracle, NeAACDecHandle handle, int channels)
{
    NeAACDecStruct *decoder = (NeAACDecStruct *)handle;
    int ch, i;

    memset(oracle, 0, sizeof(*oracle));
    oracle->channels = channels;
    oracle->fb = filter_bank_init(1024);
    oracle->start_frame = decoder->frame;
    oracle->sample_rate = get_sample_rate(decoder->sf_index);

    for (ch = 0; ch < channels; ch++) {
        int dch = decoder->internal_channel[ch];
        oracle->decoder_channel[ch] = dch;
        memcpy(oracle->base[ch], decoder->conceal_spectrum[dch],
               sizeof(oracle->base[ch]));
        memcpy(oracle->overlap[ch], decoder->fb_intermed[dch],
               sizeof(oracle->overlap[ch]));
        oracle->history_sequence[ch] =
            decoder->conceal_history_sequence[dch];
        oracle->last_sequence[ch] =
            decoder->conceal_synthesis_sequence[ch];
        oracle->window_shape[ch] =
            decoder->conceal_history_shape[dch];
        oracle->previous_shape[ch] =
            decoder->window_shape_prev[dch];
    }

    for (i = 0; i < channels; i++) {
        if (oracle->last_sequence[i] != oracle->history_sequence[i])
            oracle->last_sequence[i] = oracle->history_sequence[i];
    }
}

static void oracle_free(Oracle *oracle)
{
    if (oracle->fb)
        filter_bank_end(oracle->fb);
    oracle->fb = NULL;
}

static void oracle_step(Oracle *oracle, uint32_t k)
{
    int ch, i;
    double gain = (k >= 32) ? 0.0 : pow(0.75, (double)k);

    for (ch = 0; ch < oracle->channels; ch++) {
        uint8_t next = advance_sequence(oracle->last_sequence[ch]);
        int geometry_matches =
            sequence_is_short(oracle->history_sequence[ch]) ==
            sequence_is_short(next);

        if (gain != 0.0 && geometry_matches) {
            for (i = 0; i < 1024; i++)
                oracle->work[ch][i] =
                    (real_t)((double)oracle->base[ch][i] * gain);
        } else {
            memset(oracle->work[ch], 0, sizeof(oracle->work[ch]));
        }

        ifilter_bank(oracle->fb, next, oracle->window_shape[ch],
                     oracle->previous_shape[ch], oracle->work[ch],
                     oracle->time_out[ch], oracle->overlap[ch], LC, 1024);
        oracle->previous_shape[ch] = oracle->window_shape[ch];
        oracle->last_sequence[ch] = next;
    }
}

static int close_to_reference(double actual, double expected)
{
    double tolerance = 2e-5 * (1.0 + fabs(expected));
    return fabs(actual - expected) <= tolerance;
}

static int verify_concealment(Oracle *oracle,
                              NeAACDecHandle handle,
                              NeAACDecFrameInfo *info,
                              void *pcm,
                              int output_format,
                              uint32_t k,
                              uint32_t old_r1,
                              uint32_t old_r2)
{
    NeAACDecStruct *decoder = (NeAACDecStruct *)handle;
    int ch, i;

    if (info->error != 0) {
        fprintf(stderr, "k=%u unexpected error %u\n", k, info->error);
        return 1;
    }
    if (info->bytesconsumed != 0 || info->samples != 1024UL * oracle->channels ||
        info->channels != oracle->channels ||
        (unsigned long)info->samplerate != oracle->sample_rate ||
        pcm == NULL) {
        fprintf(stderr, "k=%u bad FrameInfo\n", k);
        return 2;
    }
    if (info->channel_position[0] !=
            (oracle->channels == 1 ? FRONT_CHANNEL_CENTER : FRONT_CHANNEL_LEFT) ||
        (oracle->channels == 2 &&
         info->channel_position[1] != FRONT_CHANNEL_RIGHT)) {
        fprintf(stderr, "k=%u bad channel positions\n", k);
        return 3;
    }
    if (decoder->__r1 != old_r1 || decoder->__r2 != old_r2) {
        fprintf(stderr, "k=%u PNS/random state changed\n", k);
        return 4;
    }
    if (decoder->frame != oracle->start_frame + k) {
        fprintf(stderr, "k=%u frame count %u expected %u\n", k,
                decoder->frame, oracle->start_frame + k);
        return 5;
    }
    if (decoder->conceal_loss_count != k) {
        fprintf(stderr, "k=%u loss count %u\n", k,
                decoder->conceal_loss_count);
        return 6;
    }

    if (output_format == FAAD_FMT_FLOAT) {
        const float *float_pcm = (const float *)pcm;
        for (i = 0; i < 1024; i++) {
            for (ch = 0; ch < oracle->channels; ch++) {
                double expected =
                    (double)oracle->time_out[ch][i] / 32768.0;
                double actual = float_pcm[i * oracle->channels + ch];
                if (!close_to_reference(actual, expected)) {
                    fprintf(stderr, "float PCM mismatch k=%u i=%d ch=%d "
                            "actual=%.9g expected=%.9g\n", k, i, ch,
                            actual, expected);
                    return 10;
                }
            }
        }
    } else {
        const int16_t *short_pcm = (const int16_t *)pcm;
        for (i = 0; i < 1024; i++) {
            for (ch = 0; ch < oracle->channels; ch++) {
                double raw = oracle->time_out[ch][i];
                long expected_int;
                int16_t actual_int = short_pcm[i * oracle->channels + ch];
                if (raw >= 32767.0) expected_int = 32767;
                else if (raw <= -32768.0) expected_int = -32768;
                else expected_int = lrint(raw);
                if ((long)actual_int != expected_int) {
                    double expected = (double)expected_int / 32768.0;
                    double actual = (double)actual_int / 32768.0;
                    if (!close_to_reference(actual, expected)) {
                        fprintf(stderr, "16-bit PCM mismatch k=%u i=%d ch=%d "
                                "actual=%d expected=%ld raw=%.9g\n", k, i, ch,
                                actual_int, expected_int, raw);
                        return 11;
                    }
                }
            }
        }
    }

    for (ch = 0; ch < oracle->channels; ch++) {
        int dch = oracle->decoder_channel[ch];
        for (i = 0; i < 1024; i++) {
            double actual = decoder->fb_intermed[dch][i];
            double expected = oracle->overlap[ch][i];
            if (!close_to_reference(actual, expected)) {
                fprintf(stderr, "overlap mismatch k=%u ch=%d i=%d "
                        "actual=%.9g expected=%.9g\n", k, ch, i,
                        actual, expected);
                return 12;
            }
        }
        if (decoder->window_shape_prev[dch] != oracle->previous_shape[ch]) {
            fprintf(stderr, "previous window shape mismatch k=%u ch=%d\n", k, ch);
            return 13;
        }
        if (decoder->conceal_synthesis_sequence[ch] !=
            oracle->last_sequence[ch]) {
            fprintf(stderr, "synthesis sequence mismatch k=%u ch=%d\n", k, ch);
            return 14;
        }
    }

    return 0;
}

static int run_conceal_oracle_case(int anchor, uint32_t calls, int format,
                                   int use_buffer2)
{
    TestAU aus[MAX_TEST_AUS];
    int au_count = 0;
    NeAACDecHandle handle;
    Oracle oracle;
    uint32_t k;
    int rc = 0;
    void *caller_buffer = NULL;

    if (!load_normal_aus(aus, MAX_TEST_AUS, &au_count))
        return 1;
    handle = make_48k_stereo(format);
    if (!handle)
        return 2;
    if (!decode_aus(handle, aus, 0, anchor)) {
        NeAACDecClose(handle);
        return 3;
    }

    oracle_capture(&oracle, handle, 2);
    if (use_buffer2) {
        unsigned long bytes = 1024UL * 2 *
            (format == FAAD_FMT_16BIT ? 2 : 4);
        caller_buffer = malloc(bytes);
        if (!caller_buffer) {
            oracle_free(&oracle);
            NeAACDecClose(handle);
            return 4;
        }
    }

    for (k = 1; k <= calls && rc == 0; k++) {
        NeAACDecFrameInfo info;
        void *pcm;
        uint32_t r1 = ((NeAACDecStruct *)handle)->__r1;
        uint32_t r2 = ((NeAACDecStruct *)handle)->__r2;

        oracle_step(&oracle, k);
        if (use_buffer2) {
            void *buffer_pointer = caller_buffer;
            unsigned long bytes = 1024UL * 2 *
                (format == FAAD_FMT_16BIT ? 2 : 4);
            pcm = NeAACDecConceal2(handle, &info, &buffer_pointer, bytes);
        } else {
            pcm = NeAACDecConceal(handle, &info);
        }
        rc = verify_concealment(&oracle, handle, &info, pcm, format, k, r1, r2);
    }

    free(caller_buffer);
    oracle_free(&oracle);
    NeAACDecClose(handle);
    return rc;
}

/* Minimal legal mono raw AAC-LC frame.  max_sfb=1 is one PNS band; optional
 * TNS contains a real order-one filter over that band. */
static size_t make_mono_au(TestAU *au, int window_sequence,
                           int window_shape, int tns_present)
{
    struct bit_writer { unsigned char *data; size_t bit_count; } bw;
    int short_window = window_sequence == EIGHT_SHORT_SEQUENCE;

    memset(au, 0, sizeof(*au));
    bw.data = au->data;
    bw.bit_count = 0;

    {
        uint32_t bits_stack[1];
        (void)bits_stack;
    }
#define PUT_BITS(value, nbits) do { \
        uint32_t put_value_ = (uint32_t)(value); \
        int put_n_ = (nbits); \
        int put_i_; \
        for (put_i_ = put_n_ - 1; put_i_ >= 0; put_i_--) { \
            size_t bitpos_ = bw.bit_count++; \
            if ((put_value_ >> put_i_) & 1u) \
                bw.data[bitpos_ / 8] |= (unsigned char)(1u << (7 - bitpos_ % 8)); \
        } \
    } while (0)

    PUT_BITS(ID_SCE, 3);
    PUT_BITS(0, 4);                  /* element instance tag */
    PUT_BITS(160, 8);                /* global gain */
    PUT_BITS(0, 1);                  /* ICS reserved bit */
    PUT_BITS(window_sequence, 2);
    PUT_BITS(window_shape, 1);
    if (short_window) {
        PUT_BITS(1, 4);              /* max_sfb */
        PUT_BITS(127, 7);            /* group all eight windows */
    } else {
        PUT_BITS(1, 6);              /* max_sfb */
    }
    if (!short_window)
        PUT_BITS(0, 1);              /* predictor_data_present */

    PUT_BITS(NOISE_HCB, 4);          /* one PNS section */
    PUT_BITS(1, short_window ? 3 : 5);
    PUT_BITS(256, 9);                /* first PNS energy delta: 0 */

    PUT_BITS(0, 1);                  /* pulse_data_present */
    PUT_BITS(tns_present ? 1 : 0, 1);
    if (tns_present) {
        if (!short_window) {
            PUT_BITS(1, 2);          /* n_filt */
            PUT_BITS(0, 1);          /* coef_res */
            PUT_BITS(49, 6);         /* cover from first SWB */
            PUT_BITS(1, 5);          /* order */
            PUT_BITS(0, 1);          /* direction */
            PUT_BITS(0, 1);          /* coef_compress */
            PUT_BITS(2, 3);          /* nonzero coefficient index */
        } else {
            int w;
            for (w = 0; w < 8; w++)
                PUT_BITS(0, 1);      /* n_filt = 0 for each short window */
        }
    }
    PUT_BITS(0, 1);                  /* gain_control_data_present */
    PUT_BITS(7, 3);                  /* ID_END */
    while (bw.bit_count % 8)
        PUT_BITS(0, 1);

#undef PUT_BITS
    au->len = (bw.bit_count + 7) / 8;
    return au->len;
}

static int case_spectrum_fixture_capture(void)
{
    TestAU aus[MAX_TEST_AUS];
    int au_count = 0, i;
    NeAACDecHandle handle;
    NeAACDecStruct *decoder;

    TASK_CHECK(load_normal_aus(aus, MAX_TEST_AUS, &au_count));
    handle = make_48k_stereo(FAAD_FMT_FLOAT);
    TASK_CHECK(handle != NULL);
    TASK_CHECK(decode_aus(handle, aus, 0, 4));
    decoder = (NeAACDecStruct *)handle;
    TASK_CHECK(decoder->conceal_history_valid);
    TASK_CHECK(decoder->conceal_channels == 2);
    TASK_CHECK(decoder->conceal_capture_mask == 3);

    /* The cache is spectral, not PCM: values are on the MDCT coefficient
       scale and include large post-PNS/TNS coefficients. */
    for (i = 0; i < 2; i++) {
        int nonzero = 0, j;
        double sum_abs = 0.0;
        for (j = 0; j < 1024; j++) {
            if (decoder->conceal_spectrum[i][j] != 0.0f)
                nonzero++;
            sum_abs += fabs(decoder->conceal_spectrum[i][j]);
        }
        TASK_CHECK(nonzero > 100);
        TASK_CHECK(sum_abs > 10000.0);
    }

    NeAACDecClose(handle);
    return 0;
}

static int case_spectrum_synthetic_pns_tns(void)
{
    TestAU au;
    TestAU au_without_tns;
    NeAACDecHandle handle = make_48k_mono(FAAD_FMT_FLOAT);
    NeAACDecHandle control_handle = make_48k_mono(FAAD_FMT_FLOAT);
    NeAACDecFrameInfo info;
    NeAACDecStruct *decoder;
    NeAACDecStruct *control_decoder;
    int i, nonzero = 0, tns_changed = 0;
    double sum_abs = 0.0;
    uint32_t r1_before, r2_before;

    TASK_CHECK(handle != NULL);
    make_mono_au(&au, ONLY_LONG_SEQUENCE, 0, 1);
    decoder = (NeAACDecStruct *)handle;
    r1_before = decoder->__r1;
    r2_before = decoder->__r2;
    NeAACDecDecode(handle, &info, au.data, au.len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 0); /* first good AU is still suppressed */
    TASK_CHECK(decoder->conceal_history_valid);
    TASK_CHECK(decoder->conceal_capture_mask == 1);
    TASK_CHECK(decoder->__r1 != r1_before || decoder->__r2 != r2_before);

    for (i = 0; i < 1024; i++) {
        if (decoder->conceal_spectrum[0][i] != 0.0f)
            nonzero++;
        sum_abs += fabs(decoder->conceal_spectrum[0][i]);
    }
    /* The only coded spectral section is PNS; therefore these nonzero
       quantized-zero coefficients can only exist after PNS/TNS processing. */
    TASK_CHECK(nonzero >= 4);
    TASK_CHECK(sum_abs > 1.0);
    TASK_CHECK(NeAACDecGetErrorMessage(CONCEAL_ERROR_UNAVAILABLE) != NULL);

    /* Same PNS frame without TNS starts from identical RNG state; the real
       TNS stage must measurably alter the post-processing spectrum. */
    TASK_CHECK(control_handle != NULL);
    make_mono_au(&au_without_tns, ONLY_LONG_SEQUENCE, 0, 0);
    NeAACDecDecode(control_handle, &info, au_without_tns.data,
                   au_without_tns.len);
    TASK_CHECK(info.error == 0);
    control_decoder = (NeAACDecStruct *)control_handle;
    for (i = 0; i < 1024; i++) {
        if (fabs((double)decoder->conceal_spectrum[0][i] -
                 (double)control_decoder->conceal_spectrum[0][i]) > 1e-6)
            tns_changed++;
    }
    TASK_CHECK(tns_changed > 0);

    NeAACDecClose(control_handle);
    NeAACDecClose(handle);
    return 0;
}

static int case_spectrum_legal_windows(void)
{
    static const uint8_t sequences[4] = {
        ONLY_LONG_SEQUENCE, LONG_START_SEQUENCE,
        EIGHT_SHORT_SEQUENCE, LONG_STOP_SEQUENCE
    };
    TestAU au;
    NeAACDecHandle handle = make_48k_mono(FAAD_FMT_FLOAT);
    NeAACDecStruct *decoder = (NeAACDecStruct *)handle;
    int f;

    TASK_CHECK(handle != NULL);
    for (f = 0; f < 4; f++) {
        NeAACDecFrameInfo info;
        make_mono_au(&au, sequences[f], (uint8_t)(f & 1), f == 0);
        NeAACDecDecode(handle, &info, au.data, au.len);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(decoder->conceal_history_valid);
        TASK_CHECK(decoder->conceal_history_sequence[0] == sequences[f]);
        TASK_CHECK(decoder->conceal_history_shape[0] == (uint8_t)(f & 1));
    }
    NeAACDecClose(handle);
    return 0;
}

static int case_spectrum_44k1_mono(void)
{
    static const unsigned char asc[2] = { 0x12, 0x08 };
    TestAU au;
    NeAACDecHandle handle = make_handle(FAAD_FMT_FLOAT, asc, sizeof(asc));
    NeAACDecFrameInfo info;
    Oracle oracle;
    uint32_t r1, r2;
    int k, rc;

    TASK_CHECK(handle != NULL);
    make_mono_au(&au, ONLY_LONG_SEQUENCE, 1, 1);
    NeAACDecDecode(handle, &info, au.data, au.len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samplerate == 44100);
    TASK_CHECK(info.channels == 1);
    TASK_CHECK(((NeAACDecStruct *)handle)->sf_index == 4);
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);

    oracle_capture(&oracle, handle, 1);
    TASK_CHECK(oracle.sample_rate == 44100);
    for (k = 1; k <= 2; k++) {
        void *pcm;
        r1 = ((NeAACDecStruct *)handle)->__r1;
        r2 = ((NeAACDecStruct *)handle)->__r2;
        oracle_step(&oracle, (uint32_t)k);
        pcm = NeAACDecConceal(handle, &info);
        rc = verify_concealment(&oracle, handle, &info, pcm,
                                FAAD_FMT_FLOAT, (uint32_t)k, r1, r2);
        TASK_CHECK(rc == 0);
    }
    oracle_free(&oracle);
    NeAACDecClose(handle);
    return 0;
}

static int case_continuity_first_good_au(void)
{
    TASK_CHECK(run_conceal_oracle_case(0, 1, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_single_float(void)
{
    TASK_CHECK(run_conceal_oracle_case(6, 1, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_repeated_float(void)
{
    TASK_CHECK(run_conceal_oracle_case(6, 6, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_beyond_32(void)
{
    TASK_CHECK(run_conceal_oracle_case(6, 40, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_short_repeated(void)
{
    TASK_CHECK(run_conceal_oracle_case(1, 5, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_long_start_to_short_zero(void)
{
    /* AU 3 in normal.aac is LONG_START.  The first concealed AU must be
       EIGHT_SHORT with a zero spectral vector while retaining overlap. */
    TASK_CHECK(run_conceal_oracle_case(3, 2, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_long_stop_to_long(void)
{
    TASK_CHECK(run_conceal_oracle_case(2, 2, FAAD_FMT_FLOAT, 0) == 0);
    return 0;
}

static int case_continuity_synthetic_short(void)
{
    TestAU au;
    NeAACDecHandle handle = make_48k_mono(FAAD_FMT_FLOAT);
    Oracle oracle;
    NeAACDecFrameInfo info;
    uint32_t r1, r2;
    int k;

    TASK_CHECK(handle != NULL);
    make_mono_au(&au, EIGHT_SHORT_SEQUENCE, 1, 0);
    NeAACDecDecode(handle, &info, au.data, au.len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);

    oracle_capture(&oracle, handle, 1);
    for (k = 1; k <= 4; k++) {
        void *pcm;
        int rc;
        r1 = ((NeAACDecStruct *)handle)->__r1;
        r2 = ((NeAACDecStruct *)handle)->__r2;
        oracle_step(&oracle, (uint32_t)k);
        pcm = NeAACDecConceal(handle, &info);
        rc = verify_concealment(&oracle, handle, &info, pcm,
                                FAAD_FMT_FLOAT, (uint32_t)k, r1, r2);
        TASK_CHECK(rc == 0);
    }
    oracle_free(&oracle);
    NeAACDecClose(handle);
    return 0;
}

static int case_continuity_next_good_recovers(void)
{
    TestAU aus[MAX_TEST_AUS];
    int au_count = 0;
    NeAACDecHandle handle;
    Oracle oracle;
    NeAACDecFrameInfo info;
    void *pcm;
    uint32_t r1, r2;
    int rc;

    TASK_CHECK(load_normal_aus(aus, MAX_TEST_AUS, &au_count));
    handle = make_48k_stereo(FAAD_FMT_FLOAT);
    TASK_CHECK(handle != NULL);
    TASK_CHECK(decode_aus(handle, aus, 0, 5));

    r1 = ((NeAACDecStruct *)handle)->__r1;
    r2 = ((NeAACDecStruct *)handle)->__r2;
    oracle_capture(&oracle, handle, 2);
    oracle_step(&oracle, 1);
    pcm = NeAACDecConceal(handle, &info);
    TASK_CHECK(verify_concealment(&oracle, handle, &info, pcm,
                                 FAAD_FMT_FLOAT, 1, r1, r2) == 0);
    oracle_step(&oracle, 2);
    r1 = ((NeAACDecStruct *)handle)->__r1;
    r2 = ((NeAACDecStruct *)handle)->__r2;
    pcm = NeAACDecConceal(handle, &info);
    TASK_CHECK(verify_concealment(&oracle, handle, &info, pcm,
                                 FAAD_FMT_FLOAT, 2, r1, r2) == 0);
    oracle_free(&oracle);

    /* The next real AU continues from the advanced overlap/frame state. */
    pcm = NeAACDecDecode(handle, &info, aus[6].data, aus[6].len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_loss_count == 0);
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);

    oracle_capture(&oracle, handle, 2);
    r1 = ((NeAACDecStruct *)handle)->__r1;
    r2 = ((NeAACDecStruct *)handle)->__r2;
    oracle_step(&oracle, 1);
    pcm = NeAACDecConceal(handle, &info);
    rc = verify_concealment(&oracle, handle, &info, pcm,
                            FAAD_FMT_FLOAT, 1, r1, r2);
    TASK_CHECK(rc == 0);

    oracle_free(&oracle);
    NeAACDecClose(handle);
    return 0;
}

static int case_continuity_failure_and_seek_rebuild(void)
{
    TestAU aus[MAX_TEST_AUS];
    int au_count = 0;
    NeAACDecHandle handle;
    NeAACDecFrameInfo info;
    Oracle oracle;
    unsigned char bad_au[16];
    void *pcm;
    uint32_t r1, r2;
    int rc;

    TASK_CHECK(load_normal_aus(aus, MAX_TEST_AUS, &au_count));
    handle = make_48k_stereo(FAAD_FMT_FLOAT);
    TASK_CHECK(handle != NULL);
    TASK_CHECK(decode_aus(handle, aus, 0, 2));
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);

    memset(bad_au, 0xff, sizeof(bad_au));
    NeAACDecDecode(handle, &info, bad_au, sizeof(bad_au));
    TASK_CHECK(info.error != 0);
    TASK_CHECK(!((NeAACDecStruct *)handle)->conceal_history_valid);
    NeAACDecConceal(handle, &info);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);

    TASK_CHECK(decode_aus(handle, aus, 6, 6));
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);

    NeAACDecPostSeekReset(handle, -1);
    TASK_CHECK(!((NeAACDecStruct *)handle)->conceal_history_valid);
    NeAACDecConceal(handle, &info);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    TASK_CHECK(decode_aus(handle, aus, 7, 7));
    TASK_CHECK(((NeAACDecStruct *)handle)->conceal_history_valid);
    oracle_capture(&oracle, handle, 2);
    r1 = ((NeAACDecStruct *)handle)->__r1;
    r2 = ((NeAACDecStruct *)handle)->__r2;
    oracle_step(&oracle, 1);
    pcm = NeAACDecConceal(handle, &info);
    rc = verify_concealment(&oracle, handle, &info, pcm,
                            FAAD_FMT_FLOAT, 1, r1, r2);
    TASK_CHECK(rc == 0);
    oracle_free(&oracle);
    NeAACDecClose(handle);
    return 0;
}

static int case_continuity_invalid_state(void)
{
    NeAACDecHandle handle = NeAACDecOpen();
    NeAACDecFrameInfo info;
    static const unsigned char asc32[2] = { 0x13, 0x10 };
    TestAU au;

    TASK_CHECK(handle != NULL);
    NeAACDecConceal(handle, &info);
    TASK_CHECK(info.error == CONCEAL_ERROR_UNAVAILABLE);
    TASK_CHECK(info.samples == 0);
    TASK_CHECK(NeAACDecGetErrorMessage(info.error) != NULL);
    NeAACDecClose(handle);

    handle = make_48k_stereo(FAAD_FMT_FLOAT);
    TASK_CHECK(handle != NULL);
    NeAACDecInit2(handle, (unsigned char *)asc32, 2,
                  &(unsigned long){0}, &(unsigned char){0});
    make_mono_au(&au, ONLY_LONG_SEQUENCE, 0, 0);
    NeAACDecDecode(handle, &info, au.data, au.len);
    /* The ASC changed this handle to an out-of-range 32 kHz configuration. */
    NeAACDecConceal(handle, &info);
    TASK_CHECK(info.error == CONCEAL_ERROR_UNAVAILABLE);
    TASK_CHECK(info.samples == 0);
    NeAACDecClose(handle);

    return 0;
}

static int case_buffers_float_conceal2(void)
{
    TASK_CHECK(run_conceal_oracle_case(4, 3, FAAD_FMT_FLOAT, 1) == 0);
    return 0;
}

static int snapshot_state(NeAACDecHandle handle,
                          uint32_t *frame,
                          uint32_t *loss,
                          uint32_t *r1,
                          uint32_t *r2,
                          real_t spectrum[2][1024],
                          real_t overlap[2][1024])
{
    NeAACDecStruct *d = (NeAACDecStruct *)handle;
    int ch;
    *frame = d->frame;
    *loss = d->conceal_loss_count;
    *r1 = d->__r1;
    *r2 = d->__r2;
    for (ch = 0; ch < 2; ch++) {
        memcpy(spectrum[ch], d->conceal_spectrum[ch], 1024 * sizeof(real_t));
        memcpy(overlap[ch], d->fb_intermed[ch], 1024 * sizeof(real_t));
    }
    return 0;
}

static int assert_state_same(NeAACDecHandle handle,
                             uint32_t frame,
                             uint32_t loss,
                             uint32_t r1,
                             uint32_t r2,
                             real_t spectrum[2][1024],
                             real_t overlap[2][1024])
{
    NeAACDecStruct *d = (NeAACDecStruct *)handle;
    int ch;
    TASK_CHECK(d->frame == frame);
    TASK_CHECK(d->conceal_loss_count == loss);
    TASK_CHECK(d->__r1 == r1 && d->__r2 == r2);
    for (ch = 0; ch < 2; ch++) {
        TASK_CHECK(memcmp(d->conceal_spectrum[ch], spectrum[ch],
                          1024 * sizeof(real_t)) == 0);
        TASK_CHECK(memcmp(d->fb_intermed[ch], overlap[ch],
                          1024 * sizeof(real_t)) == 0);
    }
    return 0;
}

static int case_buffers_short_retry_atomic(void)
{
    TestAU aus[MAX_TEST_AUS];
    int au_count = 0;
    NeAACDecHandle handle;
    NeAACDecFrameInfo info;
    unsigned char tiny_byte = 0;
    void *tiny_pointer = &tiny_byte;
    unsigned char buffer[256];
    void *buffer_pointer = buffer;
    void *null_pointer = NULL;
    uint32_t frame, loss, r1, r2;
    real_t spectrum[2][1024], overlap[2][1024];
    Oracle oracle;
    void *pcm;
    int rc;

    TASK_CHECK(load_normal_aus(aus, MAX_TEST_AUS, &au_count));
    handle = make_48k_stereo(FAAD_FMT_FLOAT);
    TASK_CHECK(handle != NULL);
    TASK_CHECK(NeAACDecConceal2(handle, &info, &tiny_pointer, 1) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    TASK_CHECK(decode_aus(handle, aus, 0, 3));
    snapshot_state(handle, &frame, &loss, &r1, &r2, spectrum, overlap);
    memset(buffer, 0xa5, sizeof(buffer));

    pcm = NeAACDecConceal2(handle, &info, &buffer_pointer, sizeof(buffer));
    TASK_CHECK(pcm == NULL);
    TASK_CHECK(info.error == 27);
    TASK_CHECK(info.samples == 0);
    TASK_CHECK(assert_state_same(handle, frame, loss, r1, r2,
                                spectrum, overlap) == 0);
    {
        size_t i;
        for (i = 0; i < sizeof(buffer); i++)
            TASK_CHECK(buffer[i] == 0xa5);
    }

    TASK_CHECK(NeAACDecConceal2(handle, &info, NULL, 8192) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    TASK_CHECK(assert_state_same(handle, frame, loss, r1, r2,
                                spectrum, overlap) == 0);
    TASK_CHECK(NeAACDecConceal2(handle, &info, &null_pointer, 8192) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    TASK_CHECK(assert_state_same(handle, frame, loss, r1, r2,
                                spectrum, overlap) == 0);
    TASK_CHECK(NeAACDecConceal2(handle, &info, &buffer_pointer, 0) == NULL);
    TASK_CHECK(info.error == 27 && info.samples == 0);
    TASK_CHECK(assert_state_same(handle, frame, loss, r1, r2,
                                spectrum, overlap) == 0);

    /* Enough capacity now: exactly one successful k=1 call, as if none of
       the rejected attempts had happened. */
    oracle_capture(&oracle, handle, 2);
    oracle_step(&oracle, 1);
    {
        void *big = malloc(8192);
        void *big_pointer = big;
        TASK_CHECK(big != NULL);
        pcm = NeAACDecConceal2(handle, &info, &big_pointer, 8192);
        rc = verify_concealment(&oracle, handle, &info, pcm,
                                FAAD_FMT_FLOAT, 1, r1, r2);
        TASK_CHECK(rc == 0);
        free(big);
    }
    oracle_free(&oracle);
    NeAACDecClose(handle);
    return 0;
}

static int case_buffers_16bit_and_both_entries(void)
{
    TASK_CHECK(run_conceal_oracle_case(5, 2, FAAD_FMT_16BIT, 0) == 0);
    TASK_CHECK(run_conceal_oracle_case(5, 2, FAAD_FMT_16BIT, 1) == 0);
    return 0;
}

const struct task_case task_cases[] = {
    { "spectrum", "fixture_capture", case_spectrum_fixture_capture },
    { "spectrum", "synthetic_pns_tns", case_spectrum_synthetic_pns_tns },
    { "spectrum", "legal_windows", case_spectrum_legal_windows },
    { "spectrum", "mono_44100", case_spectrum_44k1_mono },
    { "continuity", "after_priming_good_au",
      case_continuity_first_good_au },
    { "continuity", "single_float", case_continuity_single_float },
    { "continuity", "repeated_float", case_continuity_repeated_float },
    { "continuity", "beyond_32_zero", case_continuity_beyond_32 },
    { "continuity", "short_repeated", case_continuity_short_repeated },
    { "continuity", "long_start_to_short_zero",
      case_continuity_long_start_to_short_zero },
    { "continuity", "long_stop_to_long", case_continuity_long_stop_to_long },
    { "continuity", "synthetic_short", case_continuity_synthetic_short },
    { "continuity", "next_good_recovers", case_continuity_next_good_recovers },
    { "continuity", "failure_and_seek_rebuild",
      case_continuity_failure_and_seek_rebuild },
    { "continuity", "invalid_state", case_continuity_invalid_state },
    { "buffers", "float_conceal2_oracle", case_buffers_float_conceal2 },
    { "buffers", "short_retry_is_atomic", case_buffers_short_retry_atomic },
    { "buffers", "pcm16_and_buffer_entries",
      case_buffers_16bit_and_both_entries },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
