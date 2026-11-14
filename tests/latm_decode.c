/* LOAS/LATM regression tests for NeAACDecInit/NeAACDecDecode/NeAACDecDecode2.
**
** Transport frames are built independently from the raw access units of
** task_support/tone.aac and the decoded PCM is compared against the
** reference produced by NeAACDecInit2 + raw access unit decoding.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "neaacdec.h"
#include "task_support/check.h"
#include "latm_util.h"

#define TONE_AAC "task_support/tone.aac"
#define SINGLE_LOAS "task_support/single.loas"

#define FRAME_CAP 16384
#define PCM_CAP 16384

static const uint8_t ASC_MONO[2] = { 0x12, 0x08 };   /* AAC-LC 44100 Hz mono */
static const uint8_t ASC_STEREO[2] = { 0x12, 0x10 }; /* AAC-LC 44100 Hz stereo */
static const uint8_t ASC_SBR[2] = { 0x29, 0x08 };    /* object type 5 (SBR) */

static latm_au_list g_aus;
static float g_ref_float[PCM_CAP];
static unsigned long g_ref_float_samples;
static int16_t g_ref_16[PCM_CAP];
static unsigned long g_ref_16_samples;
static int g_ref_ready = 0;

/* AU 0 output is suppressed by the stream level first frame behaviour,
** AU i (i >= 1) starts at offset (i-1)*1024 in the reference buffers. */
static unsigned long ref_off(int au_index)
{
    return (unsigned long)(au_index - 1) * 1024;
}

static int decode_aus_to_buffer(int output_format, void *dst,
                                unsigned long *total_samples)
{
    NeAACDecHandle h;
    NeAACDecConfigurationPtr cfg;
    unsigned long rate = 0, total = 0;
    unsigned char ch = 0;
    int i;

    h = NeAACDecOpen();
    if (!h)
        return 1;
    cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = (unsigned char)output_format;
    cfg->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(h, cfg))
    {
        NeAACDecClose(h);
        return 1;
    }
    if (NeAACDecInit2(h, (unsigned char *)ASC_MONO, 2, &rate, &ch) != 0 ||
        rate != 44100 || ch != 1)
    {
        NeAACDecClose(h);
        return 1;
    }
    for (i = 0; i < g_aus.count; i++)
    {
        NeAACDecFrameInfo info;
        void *pcm = NeAACDecDecode(h, &info, g_aus.au[i].data,
                                   (unsigned long)g_aus.au[i].len);
        if (info.error != 0)
        {
            NeAACDecClose(h);
            return 1;
        }
        if (info.samples > 0)
        {
            if (!pcm || total + info.samples > PCM_CAP)
            {
                NeAACDecClose(h);
                return 1;
            }
            if (output_format == FAAD_FMT_FLOAT)
                memcpy((float *)dst + total, pcm, info.samples * sizeof(float));
            else
                memcpy((int16_t *)dst + total, pcm, info.samples * sizeof(int16_t));
            total += info.samples;
        }
    }
    NeAACDecClose(h);
    *total_samples = total;
    return 0;
}

static int ensure_ref(void)
{
    if (g_ref_ready)
        return 0;
    if (latm_load_aus(TONE_AAC, &g_aus) != 0)
        return 1;
    if (g_aus.count < 6)
        return 1;
    if (decode_aus_to_buffer(FAAD_FMT_FLOAT, g_ref_float, &g_ref_float_samples))
        return 1;
    if (decode_aus_to_buffer(FAAD_FMT_16BIT, g_ref_16, &g_ref_16_samples))
        return 1;
    g_ref_ready = 1;
    return 0;
}

/* Collect pointers/lengths for a list of AU indices and build a frame. */
static size_t build_frame(uint8_t *out, size_t cap, const latm_mux_cfg *cfg,
                          int num_subframes, const int *au_idx, int au_count)
{
    const uint8_t *aus[64];
    size_t lens[64];
    int i;

    if (au_count > 64)
        return 0;
    for (i = 0; i < au_count; i++)
    {
        aus[i] = g_aus.au[au_idx[i]].data;
        lens[i] = g_aus.au[au_idx[i]].len;
    }
    return latm_build_frame(out, cap, cfg, num_subframes, aus, lens, au_count);
}

/* Open a handle, set the output format and initialise it from a LOAS
** frame carrying a StreamMuxConfig. Returns NULL on failure. */
static NeAACDecHandle latm_open(const uint8_t *frame, size_t len,
                                int output_format,
                                unsigned long *rate, unsigned char *ch)
{
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg;

    if (!h)
        return NULL;
    cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = (unsigned char)output_format;
    cfg->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(h, cfg))
    {
        NeAACDecClose(h);
        return NULL;
    }
    if (NeAACDecInit(h, (unsigned char *)frame, (unsigned long)len, rate, ch) != 0)
    {
        NeAACDecClose(h);
        return NULL;
    }
    return h;
}

static double pcm_energy_f(const float *pcm, unsigned long n)
{
    double e = 0;
    unsigned long i;
    for (i = 0; i < n; i++)
        e += fabs((double)pcm[i]);
    return e;
}

/* ------------------------------------------------------------------ */
/* transport group                                                     */
/* ------------------------------------------------------------------ */

/* The shipped single.loas fixture: Init from the first frame, decode the
** same frame, then a useSameStreamMux follow-up frame. */
static int t_fixture_frame(void)
{
    uint8_t *loas;
    size_t loas_len, len2;
    uint8_t frame[FRAME_CAP];
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    float *pcm;
    int au2 = 1;

    TASK_CHECK(ensure_ref() == 0);
    TASK_CHECK(g_aus.au[0].len > 255); /* payloadLengthInfo 255 accumulation */

    loas = latm_read_file(SINGLE_LOAS, &loas_len);
    TASK_CHECK(loas != NULL);

    h = latm_open(loas, loas_len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    TASK_CHECK(rate == 44100 && ch == 1);

    /* the very same first frame goes to Decode */
    pcm = (float *)NeAACDecDecode(h, &info, loas, (unsigned long)loas_len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.header_type == LATM);
    TASK_CHECK(info.bytesconsumed == loas_len);
    TASK_CHECK(info.samples == 0); /* first AU of the stream is suppressed */

    /* follow-up frame reusing the configuration */
    cfg = latm_default_cfg(NULL, 0);
    len2 = build_frame(frame, sizeof(frame), &cfg, 1, &au2, 1);
    TASK_CHECK(len2 > 0);
    pcm = (float *)NeAACDecDecode(h, &info, frame, (unsigned long)len2);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.header_type == LATM);
    TASK_CHECK(info.bytesconsumed == len2);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(pcm_energy_f(pcm, info.samples) > 1.0);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(1), 1024 * sizeof(float)) == 0);

    NeAACDecClose(h);
    free(loas);
    return 0;
}

/* Several access units inside one transport frame, decoded in order. */
static int t_multi_subframe(void)
{
    uint8_t f1[FRAME_CAP], f2[FRAME_CAP];
    size_t len1, len2;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    float *pcm;
    int au012[3] = { 0, 1, 2 };
    int au345[3] = { 3, 4, 5 };

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len1 = build_frame(f1, sizeof(f1), &cfg, 3, au012, 3);
    TASK_CHECK(len1 > 0);

    h = latm_open(f1, len1, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    TASK_CHECK(rate == 44100 && ch == 1);

    pcm = (float *)NeAACDecDecode(h, &info, f1, (unsigned long)len1);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len1);
    /* AU 0 suppressed, AU 1 and AU 2 concatenated */
    TASK_CHECK(info.samples == 2048);
    TASK_CHECK(pcm != NULL);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(1), 2048 * sizeof(float)) == 0);

    cfg = latm_default_cfg(NULL, 0);
    len2 = build_frame(f2, sizeof(f2), &cfg, 3, au345, 3);
    TASK_CHECK(len2 > 0);
    pcm = (float *)NeAACDecDecode(h, &info, f2, (unsigned long)len2);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len2);
    TASK_CHECK(info.samples == 3072);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(3), 3072 * sizeof(float)) == 0);

    NeAACDecClose(h);

    /* all ten access units of the stream inside a single transport
       frame: the concatenated output must equal the whole raw
       reference stream */
    {
        int au_all[10];
        int i;
        for (i = 0; i < 10; i++)
            au_all[i] = i;
        cfg = latm_default_cfg(ASC_MONO, 16);
        len1 = build_frame(f1, sizeof(f1), &cfg, 10, au_all, 10);
        TASK_CHECK(len1 > 0);
        h = latm_open(f1, len1, FAAD_FMT_FLOAT, &rate, &ch);
        TASK_CHECK(h != NULL);
        pcm = (float *)NeAACDecDecode(h, &info, f1, (unsigned long)len1);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(info.bytesconsumed == len1);
        TASK_CHECK(info.samples == g_ref_float_samples);
        TASK_CHECK(pcm != NULL);
        TASK_CHECK(memcmp(pcm, g_ref_float,
                          g_ref_float_samples * sizeof(float)) == 0);
        NeAACDecClose(h);
    }
    return 0;
}

/* Repeating the identical configuration is fine, changing the ASC or
** using out-of-scope configuration values must fail explicitly. */
static int t_config_repeat_and_reject(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    int au0 = 0, au1 = 1, au2 = 2;

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) != NULL || info.samples == 0);
    TASK_CHECK(info.error == 0);

    /* identical configuration repeated: accepted */
    len = build_frame(f, sizeof(f), &cfg, 1, &au1, 1);
    TASK_CHECK(len > 0);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) != NULL);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);

    /* identical configuration with a CRC present: accepted */
    cfg.crc_present = 1;
    len = build_frame(f, sizeof(f), &cfg, 1, &au2, 1);
    TASK_CHECK(len > 0);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) != NULL);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);
    NeAACDecClose(h);

    /* changed ASC (stereo instead of mono): rejected */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch); /* fresh valid handle */
    TASK_CHECK(h != NULL);
    cfg = latm_default_cfg(ASC_STEREO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au2, 1);
    TASK_CHECK(len > 0);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) == NULL);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);
    NeAACDecClose(h);

    /* SBR object type in the ASC: rejected */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    cfg = latm_default_cfg(ASC_SBR, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au2, 1);
    TASK_CHECK(len > 0);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) == NULL);
    TASK_CHECK(info.error != 0);
    NeAACDecClose(h);

    /* out-of-scope structural values: all rejected */
    {
        int k;
        latm_mux_cfg bad;
        for (k = 0; k < 5; k++)
        {
            bad = latm_default_cfg(ASC_MONO, 16);
            switch (k)
            {
            case 0: bad.num_programs = 2; break;
            case 1: bad.num_layers = 2; break;
            case 2: bad.all_same_time = 0; break;
            case 3: bad.frame_length_type = 1; break;
            case 4: bad.audio_mux_version = 1; break;
            }
            len = build_frame(f, sizeof(f), &bad, 1, &au0, 1);
            TASK_CHECK(len > 0);
            h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
            TASK_CHECK(h == NULL); /* Init must already fail */
        }
    }
    return 0;
}

/* otherData is skipped by its declared bit length. */
static int t_other_data(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    float *pcm;
    int au0 = 0, au1 = 1, au2 = 2, au3 = 3;

    TASK_CHECK(ensure_ref() == 0);

    /* 13 bits of otherData: not a whole number of bytes */
    cfg = latm_default_cfg(ASC_MONO, 16);
    cfg.other_data_bits = 13;
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    pcm = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len);
    TASK_CHECK(info.samples == 0);

    /* reuse frame: the stored otherData length (13 bits) applies */
    cfg = latm_default_cfg(NULL, 0);
    cfg.other_data_bits = 13;
    len = build_frame(f, sizeof(f), &cfg, 1, &au1, 1);
    TASK_CHECK(len > 0);
    pcm = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(1), 1024 * sizeof(float)) == 0);

    /* multi-byte escape coded otherData length (256 bits) */
    cfg = latm_default_cfg(ASC_MONO, 16);
    cfg.other_data_bits = 256;
    len = build_frame(f, sizeof(f), &cfg, 1, &au2, 1);
    TASK_CHECK(len > 0);
    pcm = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(2), 1024 * sizeof(float)) == 0);

    cfg = latm_default_cfg(NULL, 0);
    cfg.other_data_bits = 256;
    len = build_frame(f, sizeof(f), &cfg, 1, &au3, 1);
    TASK_CHECK(len > 0);
    pcm = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(3), 1024 * sizeof(float)) == 0);

    NeAACDecClose(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* continuity group                                                    */
/* ------------------------------------------------------------------ */

/* Two frames back to back in one buffer: Decode must stop exactly at the
** first frame boundary and continue with the second frame. */
static int c_two_frames_one_buffer(void)
{
    uint8_t f1[FRAME_CAP], f2[FRAME_CAP], both[2 * FRAME_CAP];
    size_t len1, len2;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    float *pcm;
    int au01[2] = { 0, 1 };
    int au23[2] = { 2, 3 };

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len1 = build_frame(f1, sizeof(f1), &cfg, 2, au01, 2);
    TASK_CHECK(len1 > 0);
    cfg = latm_default_cfg(NULL, 0);
    len2 = build_frame(f2, sizeof(f2), &cfg, 2, au23, 2);
    TASK_CHECK(len2 > 0);
    memcpy(both, f1, len1);
    memcpy(both + len1, f2, len2);

    /* Init only looks at the first frame, even with more bytes present */
    h = latm_open(both, len1 + len2, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);

    pcm = (float *)NeAACDecDecode(h, &info, both, (unsigned long)(len1 + len2));
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len1); /* frame 2 must not be consumed */
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(1), 1024 * sizeof(float)) == 0);

    pcm = (float *)NeAACDecDecode(h, &info, both + len1, (unsigned long)len2);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.bytesconsumed == len2);
    TASK_CHECK(info.samples == 2048);
    TASK_CHECK(memcmp(pcm, g_ref_float + ref_off(2), 2048 * sizeof(float)) == 0);

    NeAACDecClose(h);
    return 0;
}

/* A whole stream of LOAS frames must produce the identical PCM stream as
** raw access unit decoding after NeAACDecInit2. */
static int c_stream_matches_raw(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0, total = 0;
    unsigned char ch = 0;
    float *pcm;
    float latm_pcm[PCM_CAP];
    int au0 = 0, au123[3] = { 1, 2, 3 }, au456[3] = { 4, 5, 6 };
    int au789[3] = { 7, 8, 9 };
    int stage;

    TASK_CHECK(ensure_ref() == 0);
    TASK_CHECK(g_aus.count == 10);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);

    for (stage = 0; stage < 4; stage++)
    {
        switch (stage)
        {
        case 0: /* config (1 subframe) + AU 0 */
            cfg = latm_default_cfg(ASC_MONO, 16);
            len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
            break;
        case 1: /* config (3 subframes) + AU 1..3 */
            cfg = latm_default_cfg(ASC_MONO, 16);
            len = build_frame(f, sizeof(f), &cfg, 3, au123, 3);
            break;
        case 2: /* reuse (3 subframes), AU 4..6 */
            cfg = latm_default_cfg(NULL, 0);
            len = build_frame(f, sizeof(f), &cfg, 3, au456, 3);
            break;
        default: /* identical config repeated, AU 7..9 */
            cfg = latm_default_cfg(ASC_MONO, 16);
            len = build_frame(f, sizeof(f), &cfg, 3, au789, 3);
            break;
        }
        TASK_CHECK(len > 0);
        pcm = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(info.bytesconsumed == len);
        TASK_CHECK(total + info.samples <= PCM_CAP);
        if (info.samples > 0)
        {
            TASK_CHECK(pcm != NULL);
            memcpy(latm_pcm + total, pcm, info.samples * sizeof(float));
            total += info.samples;
        }
    }
    NeAACDecClose(h);

    TASK_CHECK(total == g_ref_float_samples);
    TASK_CHECK(total > 0);
    TASK_CHECK(pcm_energy_f(latm_pcm, total) > 1.0);
    TASK_CHECK(memcmp(latm_pcm, g_ref_float, total * sizeof(float)) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* buffers group                                                       */
/* ------------------------------------------------------------------ */

/* NeAACDecDecode2 with a caller provided buffer, including the too small
** and NULL buffer cases. */
static int b_decode2_buffers(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    static float outbuf[PCM_CAP];
    static float tinybuf[4];
    void *p;
    int au0 = 0, au1 = 1;

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 2, (int[]){ 0, 1 }, 2);
    TASK_CHECK(len > 0);

    /* caller buffer, large enough */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    p = outbuf;
    TASK_CHECK(NeAACDecDecode2(h, &info, f, (unsigned long)len, &p,
                               (unsigned long)sizeof(outbuf)) == outbuf);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(info.bytesconsumed == len);
    TASK_CHECK(memcmp(outbuf, g_ref_float + ref_off(1), 1024 * sizeof(float)) == 0);
    NeAACDecClose(h);

    /* caller buffer too small: error, no samples */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    p = tinybuf;
    TASK_CHECK(NeAACDecDecode2(h, &info, f, (unsigned long)len, &p,
                               (unsigned long)sizeof(tinybuf)) == NULL);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);
    NeAACDecClose(h);

    /* NULL caller buffer: error */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    p = NULL;
    TASK_CHECK(NeAACDecDecode2(h, &info, f, (unsigned long)len, &p,
                               (unsigned long)sizeof(outbuf)) == NULL);
    TASK_CHECK(info.error != 0);
    TASK_CHECK(info.samples == 0);
    NeAACDecClose(h);

    /* internal buffer and caller buffer agree */
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);
    {
        float *internal = (float *)NeAACDecDecode(h, &info, f, (unsigned long)len);
        TASK_CHECK(info.error == 0);
        TASK_CHECK(internal != NULL);
        TASK_CHECK(info.samples == 1024);
        TASK_CHECK(memcmp(internal, outbuf, 1024 * sizeof(float)) == 0);
    }
    NeAACDecClose(h);
    (void)au0; (void)au1;
    return 0;
}

/* Broken lengths and boundaries: every case must fail with a non-zero
** error and zero samples, without consuming bytes. */
static int b_length_errors(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    int au0 = 0, au1 = 1;
    uint8_t shortbuf[2] = { 0x56, 0xE0 };
    uint8_t badsync[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t zeromux[3] = { 0x56, 0xE0, 0x00 };
    uint8_t reuse_no_config[8] = { 0x56, 0xE0, 0x02, 0x80, 0x00, 0, 0, 0 };
    /* useSameStreamMux, then payloadLengthInfo 0xFF, 0xFF, ... far beyond
       the 4 byte mux */
    uint8_t huge_len[7] = { 0x56, 0xE0, 0x04, 0xFF, 0xFF, 0xFE, 0x00 };
    uint8_t zero_payload[5] = { 0x56, 0xE0, 0x02, 0x80, 0x00 };

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);

    /* useSameStreamMux without a previous configuration: Init fails */
    h = NeAACDecOpen();
    TASK_CHECK(h != NULL);
    TASK_CHECK(NeAACDecInit(h, reuse_no_config, sizeof(reuse_no_config),
                            &rate, &ch) != 0);
    NeAACDecClose(h);

    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL);

    /* short header */
    TASK_CHECK(NeAACDecDecode(h, &info, shortbuf, sizeof(shortbuf)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0 && info.bytesconsumed == 0);

    /* no LOAS sync */
    TASK_CHECK(NeAACDecDecode(h, &info, badsync, sizeof(badsync)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    /* zero audioMuxLengthBytes */
    TASK_CHECK(NeAACDecDecode(h, &info, zeromux, sizeof(zeromux)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    /* declared mux length larger than the provided buffer (truncated) */
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)(len - 2)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0 && info.bytesconsumed == 0);

    /* payload length running past the mux (255 accumulation overflow) */
    TASK_CHECK(NeAACDecDecode(h, &info, huge_len, sizeof(huge_len)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0 && info.bytesconsumed == 0);

    /* zero length payload */
    TASK_CHECK(NeAACDecDecode(h, &info, zero_payload, sizeof(zero_payload)) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);

    NeAACDecClose(h);

    /* otherData length exceeding the rest of the frame */
    cfg = latm_default_cfg(ASC_MONO, 16);
    cfg.other_data_bits = 4000;
    len = build_frame(f, sizeof(f), &cfg, 1, &au1, 1);
    TASK_CHECK(len > 0);
    TASK_CHECK(len > 4);
    /* shrink the declared mux length by one byte: the otherData skip now
       runs past the declared frame end */
    {
        unsigned mux = ((unsigned)(f[1] & 0x1F) << 8) | f[2];
        mux -= 1;
        f[1] = (uint8_t)(0xE0 | ((mux >> 8) & 0x1F));
        f[2] = (uint8_t)(mux & 0xFF);
        len -= 1;
    }
    h = latm_open(f, len, FAAD_FMT_FLOAT, &rate, &ch);
    TASK_CHECK(h != NULL); /* config itself is still inside the frame */
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0 && info.bytesconsumed == 0);
    NeAACDecClose(h);

    return 0;
}

/* 16 bit integer and float output are supported, everything else is
** rejected explicitly. */
static int b_output_formats(void)
{
    uint8_t f[FRAME_CAP];
    size_t len;
    latm_mux_cfg cfg;
    NeAACDecHandle h;
    NeAACDecFrameInfo info;
    unsigned long rate = 0;
    unsigned char ch = 0;
    int16_t *pcm16;
    int au0 = 0, au1 = 1;

    TASK_CHECK(ensure_ref() == 0);

    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);

    /* 16 bit output matches the raw reference */
    h = latm_open(f, len, FAAD_FMT_16BIT, &rate, &ch);
    TASK_CHECK(h != NULL);
    pcm16 = (int16_t *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 0);
    cfg = latm_default_cfg(NULL, 0);
    len = build_frame(f, sizeof(f), &cfg, 1, &au1, 1);
    TASK_CHECK(len > 0);
    pcm16 = (int16_t *)NeAACDecDecode(h, &info, f, (unsigned long)len);
    TASK_CHECK(info.error == 0);
    TASK_CHECK(info.samples == 1024);
    TASK_CHECK(pcm16 != NULL);
    TASK_CHECK(memcmp(pcm16, g_ref_16 + ref_off(1), 1024 * sizeof(int16_t)) == 0);
    NeAACDecClose(h);

    /* 24 bit output: rejected */
    cfg = latm_default_cfg(ASC_MONO, 16);
    len = build_frame(f, sizeof(f), &cfg, 1, &au0, 1);
    TASK_CHECK(len > 0);
    h = latm_open(f, len, FAAD_FMT_24BIT, &rate, &ch);
    TASK_CHECK(h != NULL);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);
    NeAACDecClose(h);

    /* double output: rejected */
    h = latm_open(f, len, FAAD_FMT_DOUBLE, &rate, &ch);
    TASK_CHECK(h != NULL);
    TASK_CHECK(NeAACDecDecode(h, &info, f, (unsigned long)len) == NULL);
    TASK_CHECK(info.error != 0 && info.samples == 0);
    NeAACDecClose(h);

    return 0;
}

const struct task_case task_cases[] = {
    { "transport", "fixture_frame", t_fixture_frame },
    { "transport", "multi_subframe", t_multi_subframe },
    { "transport", "config_repeat_and_reject", t_config_repeat_and_reject },
    { "transport", "other_data", t_other_data },
    { "continuity", "two_frames_one_buffer", c_two_frames_one_buffer },
    { "continuity", "stream_matches_raw", c_stream_matches_raw },
    { "buffers", "decode2_buffers", b_decode2_buffers },
    { "buffers", "length_errors", b_length_errors },
    { "buffers", "output_formats", b_output_formats },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
