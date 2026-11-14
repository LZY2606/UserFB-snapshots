/*
 *      Tests for the optional closed-loop ABR feedback path
 *
 * Three groups are registered with task_support/check.h:
 *   ledger        - what is counted, and when
 *   feedback      - the controller actually changes the bitrate
 *   compatibility - frame syntax, decoding, chunking, invariants
 */

#include "task_support/check.h"
#include "lame.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* deterministic signals                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state;
} rng_t;

static double
rng_gauss(rng_t * r)
{
    /* sum of uniforms, zero mean, roughly +/- 1 */
    double v = 0;
    int     i;
    for (i = 0; i < 4; ++i) {
        r->state = 1664525u * r->state + 1013904223u;
        v += ((double) ((r->state >> 8) & 0xFFFFFF) / 8388608.0) - 1.0;
    }
    return v * 0.5;
}

typedef enum {
    SIG_NOISE = 0,
    SIG_TONE,
    SIG_TRANSIENT,
    SIG_SILENCE
} sig_kind;

static void
make_pcm(short *pcm, int nsamples, int sr, sig_kind kind, int amplitude,
         int position, rng_t * rl, rng_t * rr)
{
    int     i;
    for (i = 0; i < nsamples; ++i) {
        int const idx = position + i;
        double  l = 0, r = 0;
        switch (kind) {
        case SIG_NOISE:
            l = amplitude * rng_gauss(rl);
            r = amplitude * rng_gauss(rr);
            break;
        case SIG_TONE:
            l = amplitude * sin(2.0 * 3.14159265358979323846 * 997.0 * idx / sr);
            l += amplitude * 0.33 * sin(2.0 * 3.14159265358979323846 * 3000.0 * idx / sr);
            r = amplitude * sin(2.0 * 3.14159265358979323846 * 1500.0 * idx / sr);
            r += amplitude * 0.33 * sin(2.0 * 3.14159265358979323846 * 4000.0 * idx / sr);
            break;
        case SIG_TRANSIENT:
            if ((idx % 512) < 8)
                l = amplitude * rng_gauss(rl) * 2.0;
            if ((idx % 384) < 6)
                r = amplitude * rng_gauss(rr) * 2.0;
            break;
        case SIG_SILENCE:
        default:
            break;
        }
        if (l > 32767.0)
            l = 32767.0;
        if (l < -32768.0)
            l = -32768.0;
        if (r > 32767.0)
            r = 32767.0;
        if (r < -32768.0)
            r = -32768.0;
        pcm[2 * i] = (short) l;
        pcm[2 * i + 1] = (short) r;
    }
}

/* ------------------------------------------------------------------ */
/* independent MP3 frame parser                                       */
/* ------------------------------------------------------------------ */

static const int mpeg1_kbps[16] =
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const int mpeg1_rate[4] = { 44100, 48000, 32000, 0 };

typedef struct {
    int     offset;             /* byte offset of the frame             */
    int     bytes;              /* complete frame length in bytes       */
    int     kbps;               /* bitrate index -> kbps                */
    int     is_xing;            /* first frame carrying a Xing/Info tag */
    int     channels;
    int     crc;
} frame_info;

typedef struct {
    frame_info *f;
    int     count;
    int     capacity;
} frame_list;

static void
frame_list_free(frame_list * l)
{
    free(l->f);
    l->f = NULL;
    l->count = l->capacity = 0;
}

static int
frame_list_push(frame_list * l, frame_info const *fi)
{
    if (l->count == l->capacity) {
        int     cap = l->capacity ? 2 * l->capacity : 256;
        frame_info *nf = (frame_info *) realloc(l->f, sizeof(*nf) * cap);
        if (!nf)
            return -1;
        l->f = nf;
        l->capacity = cap;
    }
    l->f[l->count++] = *fi;
    return 0;
}

/*
 * Parse a raw MP3 stream.  The Xing/Info frame is identified by its tag
 * magic and marked, it is still a physical frame but it is not audio.
 */
static int
parse_mp3(unsigned char const *buf, int size, int sr_expected, frame_list * out)
{
    int     pos = 0;

    out->f = NULL;
    out->count = out->capacity = 0;

    while (pos + 4 <= size) {
        frame_info fi;
        int     b1, b2, b3, version, layer, bitrate_index, sr_index, pad;
        int     rate, len, ch_mode, sideinfo, crc;

        if (buf[pos] != 0xFF || (buf[pos + 1] & 0xE0) != 0xE0)
            goto bad;
        b1 = buf[pos + 1];
        b2 = buf[pos + 2];
        b3 = buf[pos + 3];

        version = (b1 >> 3) & 3; /* 3 = MPEG1 */
        layer = (b1 >> 1) & 3;   /* 1 = layer III */
        if (version != 3 || layer != 1)
            goto bad;

        crc = (b1 & 1) ? 0 : 1;
        bitrate_index = (b2 >> 4) & 0xF;
        sr_index = (b2 >> 2) & 3;
        pad = (b2 >> 1) & 1;
        if (bitrate_index < 1 || bitrate_index > 14)
            goto bad;
        if (sr_index > 2)
            goto bad;
        rate = mpeg1_rate[sr_index];
        if (rate != sr_expected)
            goto bad;

        len = 144000 * mpeg1_kbps[bitrate_index] / rate + pad;
        if (len < 24 || pos + len > size)
            goto bad;

        ch_mode = (b3 >> 6) & 3;
        sideinfo = (ch_mode == 3) ? 17 : 32;
        fi.offset = pos;
        fi.bytes = len;
        fi.kbps = mpeg1_kbps[bitrate_index];
        fi.is_xing = 0;
        fi.channels = (ch_mode == 3) ? 1 : 2;
        fi.crc = crc;
        /*
         * LAME keeps the Xing/Info data at the same offset whether or not
         * error protection is enabled (it documents this explicitly), so
         * the tag starts right after the four byte header and the side
         * information, not after the optional CRC.
         */
        if (out->count == 0 && 4 + sideinfo + 4 <= len) {
            unsigned char const *tag = buf + pos + 4 + sideinfo;
            if ((tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g')
                || (tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o')) {
                fi.is_xing = 1;
            }
        }
        if (frame_list_push(out, &fi) < 0)
            goto bad;
        pos += len;
    }
    if (pos != size || out->count == 0)
        goto bad;
    return 0;

  bad:
    frame_list_free(out);
    return -1;
}

/* count of audio (non Xing/ID3) frames and their physical bits */
static void
audio_totals(frame_list const * l, int *frames_out, uint64_t *bits_out)
{
    int     i, n = 0;
    uint64_t bits = 0;
    for (i = 0; i < l->count; ++i) {
        if (l->f[i].is_xing)
            continue;
        ++n;
        bits += (uint64_t) l->f[i].bytes * 8;
    }
    *frames_out = n;
    *bits_out = bits;
}

/* average kbps over a run of audio frames */
static double
run_kbps(frame_list const * l, int start_audio, int count, int sr)
{
    int     i, seen = 0, first = -1, bits = 0;
    for (i = 0; i < l->count; ++i) {
        if (l->f[i].is_xing)
            continue;
        if (seen >= start_audio && seen < start_audio + count) {
            if (first < 0)
                first = i;
            bits += l->f[i].bytes * 8;
        }
        ++seen;
    }
    (void) first;
    if (count <= 0)
        return 0.0;
    return (double) bits / (count * 1152.0 / sr) / 1000.0;
}

static int
run_distinct(frame_list const * l, int start_audio, int count)
{
    int     i, seen = 0;
    int     used[16] = { 0 };
    int     distinct = 0;
    for (i = 0; i < l->count; ++i) {
        int     idx;
        if (l->f[i].is_xing)
            continue;
        if (seen >= start_audio && seen < start_audio + count) {
            for (idx = 1; idx <= 14; ++idx)
                if (mpeg1_kbps[idx] == l->f[i].kbps)
                    used[idx] = 1;
        }
        ++seen;
    }
    for (i = 0; i < 16; ++i)
        distinct += used[i];
    return distinct;
}

/* ------------------------------------------------------------------ */
/* encoder helpers                                                    */
/* ------------------------------------------------------------------ */

#define MAX_STREAM  (12u * 1024u * 1024u)

typedef struct {
    unsigned char *data;
    int     size;
    int     capacity;
} stream_t;

static int
stream_append(stream_t * s, unsigned char const *p, int n)
{
    if (n < 0)
        return -1;
    if (s->size + n > s->capacity) {
        int     cap = s->capacity ? 2 * s->capacity : 262144;
        unsigned char *nd;
        while (cap < s->size + n)
            cap *= 2;
        nd = (unsigned char *) realloc(s->data, (size_t) cap);
        if (!nd)
            return -1;
        s->data = nd;
        s->capacity = cap;
    }
    memcpy(s->data + s->size, p, (size_t) n);
    s->size += n;
    return 0;
}

static lame_t
open_encoder(int sr, int target, int feedback, int crc, int xing)
{
    lame_t  g = lame_init();
    if (!g)
        return NULL;
    lame_set_in_samplerate(g, sr);
    lame_set_out_samplerate(g, sr);
    lame_set_num_channels(g, 2);
    lame_set_mode(g, JOINT_STEREO);
    lame_set_VBR(g, vbr_abr);
    lame_set_VBR_mean_bitrate_kbps(g, target);
    lame_set_bWriteVbrTag(g, xing);
    lame_set_error_protection(g, crc);
    lame_set_quality(g, 2);
    if (feedback) {
        if (lame_set_abr_feedback(g, 1) != 0) {
            lame_close(g);
            return NULL;
        }
    }
    if (lame_init_params(g) < 0) {
        lame_close(g);
        return NULL;
    }
    return g;
}

/* encode a block of interleaved stereo PCM, return bytes appended */
static int
encode_block(lame_t g, stream_t * out, short *pcm, int nsamples)
{
    unsigned char tmp[16384];
    int     done = 0;
    while (done < nsamples) {
        int     n = nsamples - done;
        int     got;
        if (n > 1152)
            n = 1152;
        got = lame_encode_buffer_interleaved(g, pcm + 2 * done, n, tmp, sizeof(tmp));
        if (got < 0)
            return -1;
        if (stream_append(out, tmp, got) < 0)
            return -1;
        done += n;
    }
    return 0;
}

static int
flush_stream(lame_t g, stream_t * out)
{
    unsigned char tmp[16384];
    int     got = lame_encode_flush(g, tmp, sizeof(tmp));
    if (got < 0)
        return -1;
    return stream_append(out, tmp, got);
}

/*
 * With the Xing tag enabled, lame_encode_flush leaves an empty placeholder
 * frame at the start of the stream.  A real application replaces it with
 * the final tag, exactly like lame_get_lametag_frame() documents.  Doing it
 * here lets the frame parser see the same syntax a player would.
 */
static int
patch_xing_tag(lame_t g, stream_t * out)
{
    size_t  n;
    if (out->size <= 0)
        return -1;
    n = lame_get_lametag_frame(g, out->data, (size_t) out->size);
    if (n == 0)
        return -1;      /* tag was disabled by the encoder */
    if ((int) n > out->size)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* group: ledger                                                      */
/* ------------------------------------------------------------------ */

static int
test_ledger_empty(void)
{
    lame_abr_feedback_stats st;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    TASK_CHECK(g != NULL);

    memset(&st, 0xAA, sizeof(st));
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
    TASK_CHECK(st.frames == 0);
    TASK_CHECK(st.encoded_bits == 0);
    TASK_CHECK(st.budget_bit_samples == 0);
    TASK_CHECK(st.target_kbps == 96);

    lame_close(g);
    return 0;
}

static int
test_ledger_no_output_calls(void)
{
    lame_abr_feedback_stats a, b;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    short   pcm[2 * 64];
    rng_t   rl = { 0x1111u }, rr = { 0x2222u };
    unsigned char tmp[16384];
    int     got;

    TASK_CHECK(g != NULL);
    make_pcm(pcm, 64, 48000, SIG_NOISE, 6000, 0, &rl, &rr);

    TASK_CHECK(lame_get_abr_feedback_stats(g, &a) == 0);
    /* far fewer samples than one frame: buffered, no frame submitted */
    got = lame_encode_buffer_interleaved(g, pcm, 64, tmp, sizeof(tmp));
    TASK_CHECK(got == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &b) == 0);
    TASK_CHECK(b.frames == a.frames);
    TASK_CHECK(b.encoded_bits == a.encoded_bits);
    TASK_CHECK(b.budget_bit_samples == a.budget_bit_samples);

    lame_close(g);
    return 0;
}

static int
test_ledger_flush_frames(void)
{
    /* frames encoded to pad out the stream during flush are counted */
    lame_abr_feedback_stats before, after;
    stream_t out;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    short   pcm[2 * 1152];
    rng_t   rl = { 0x4321u }, rr = { 0x8765u };
    int     i;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 50; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 9000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(lame_get_abr_feedback_stats(g, &before) == 0);
    TASK_CHECK(flush_stream(g, &out) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &after) == 0);

    TASK_CHECK(after.frames > before.frames);
    TASK_CHECK(after.encoded_bits > before.encoded_bits);
    TASK_CHECK(after.budget_bit_samples > before.budget_bit_samples);

    /* every ledger frame is a whole physical frame in the stream */
    {
        frame_list fl;
        int     frames;
        uint64_t bits;
        TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);
        audio_totals(&fl, &frames, &bits);
        TASK_CHECK(after.frames == (uint64_t) frames);
        TASK_CHECK(after.encoded_bits == bits);
        frame_list_free(&fl);
    }
    lame_close(g);
    free(out.data);
    return 0;
}

static int
test_ledger_signed_error(void)
{
    /* the documented signed error matches the accumulated ledger numbers */
    static const int rates[2] = { 48000, 44100 };
    int     r;
    for (r = 0; r < 2; ++r) {
        int const sr = rates[r];
        lame_t  g = open_encoder(sr, 128, 1, 0, 0);
        short   pcm[2 * 1152];
        rng_t   rl = { 0x9999u }, rr = { 0xAAAAu };
        stream_t out;
        lame_abr_feedback_stats st;
        int     i;

        TASK_CHECK(g != NULL);
        memset(&out, 0, sizeof(out));
        for (i = 0; i < 400; ++i) {
            make_pcm(pcm, 1152, sr, SIG_NOISE, 10000, i * 1152, &rl, &rr);
            TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
        }
        TASK_CHECK(flush_stream(g, &out) == 0);
        TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
        /* on this amount of noise the average is close to the target, so
         * the signed error is small compared to the total budget */
        {
            double  err = (double) st.encoded_bits * sr -
                (double) st.budget_bit_samples;
            double  budget = (double) st.budget_bit_samples;
            TASK_CHECK(st.budget_bit_samples > 0);
            if (!(err > -0.05 * budget && err < 0.05 * budget)) {
                fprintf(stderr, "signed error %.0f bits on budget %.0f at %d Hz\n",
                        err, budget, sr);
            }
            TASK_CHECK(err > -0.05 * budget && err < 0.05 * budget);
        }
        lame_close(g);
        free(out.data);
    }
    return 0;
}

static int
test_ledger_matches_parser(void)
{
    static const int rates[2] = { 48000, 44100 };
    int     r;
    for (r = 0; r < 2; ++r) {
        int const sr = rates[r];
        lame_abr_feedback_stats st;
        stream_t out;
        frame_list fl;
        short   pcm[2 * 1152];
        rng_t   rl = { 0x3434u }, rr = { 0x5656u };
        int     i, frames;
        uint64_t bits;

        memset(&out, 0, sizeof(out));
        memset(&st, 0, sizeof(st));
        {
            lame_t  g = open_encoder(sr, 128, 1, 0, 0);
            TASK_CHECK(g != NULL);
            for (i = 0; i < 240; ++i) {
                make_pcm(pcm, 1152, sr, SIG_NOISE, 9000, i * 1152, &rl, &rr);
                TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
            }
            /* flush pads with audio frames, they are part of the ledger */
            TASK_CHECK(flush_stream(g, &out) == 0);
            TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
            lame_close(g);
        }

        TASK_CHECK(parse_mp3(out.data, out.size, sr, &fl) == 0);
        audio_totals(&fl, &frames, &bits);
        TASK_CHECK(st.frames == (uint64_t) frames);
        TASK_CHECK(st.encoded_bits == bits);

        /* every physical frame counted, including the padding frames */
        TASK_CHECK(st.frames >= 240);

        /* the budget is per frame: target * 1000 * frame samples */
        TASK_CHECK(st.budget_bit_samples == (uint64_t) st.frames * 128ull * 1000ull * 1152ull);
        TASK_CHECK(st.target_kbps == 128);

        frame_list_free(&fl);
        free(out.data);
    }
    return 0;
}

static int
test_ledger_excludes_xing(void)
{
    int     x;
    for (x = 0; x <= 1; ++x) {
        lame_abr_feedback_stats st;
        stream_t out;
        frame_list fl;
        short   pcm[2 * 1152];
        rng_t   rl = { 0x77u }, rr = { 0x99u };
        int     i, frames;
        uint64_t bits;
        int     xing_frames = 0;

        memset(&out, 0, sizeof(out));
        memset(&st, 0, sizeof(st));
        {
            lame_t  g = open_encoder(48000, 96, 1, 0, x);
            TASK_CHECK(g != NULL);
            for (i = 0; i < 180; ++i) {
                make_pcm(pcm, 1152, 48000, SIG_NOISE, 8000, i * 1152, &rl, &rr);
                TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
            }
            TASK_CHECK(flush_stream(g, &out) == 0);
            if (x)
                TASK_CHECK(patch_xing_tag(g, &out) == 0);
            TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
            lame_close(g);
        }

        TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);
        for (i = 0; i < fl.count; ++i)
            xing_frames += fl.f[i].is_xing;
        TASK_CHECK(x ? xing_frames == 1 : xing_frames == 0);

        audio_totals(&fl, &frames, &bits);
        TASK_CHECK(st.frames == (uint64_t) frames);
        TASK_CHECK(st.encoded_bits == bits);
        frame_list_free(&fl);
        free(out.data);
    }
    return 0;
}

static int
test_ledger_crc(void)
{
    lame_abr_feedback_stats st;
    stream_t out;
    frame_list fl;
    short   pcm[2 * 1152];
    rng_t   rl = { 0xBBu }, rr = { 0xCCu };
    int     i, frames, crc_frames = 0;
    uint64_t bits;

    memset(&out, 0, sizeof(out));
    memset(&st, 0, sizeof(st));
    {
        lame_t  g = open_encoder(44100, 112, 1, 1, 0);
        TASK_CHECK(g != NULL);
        for (i = 0; i < 150; ++i) {
            make_pcm(pcm, 1152, 44100, SIG_NOISE, 9000, i * 1152, &rl, &rr);
            TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
        }
        TASK_CHECK(flush_stream(g, &out) == 0);
        TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
        lame_close(g);
    }
    TASK_CHECK(parse_mp3(out.data, out.size, 44100, &fl) == 0);
    for (i = 0; i < fl.count; ++i) {
        crc_frames += fl.f[i].crc;
        TASK_CHECK(fl.f[i].crc == 1);
    }
    TASK_CHECK(crc_frames == fl.count);
    audio_totals(&fl, &frames, &bits);
    TASK_CHECK(st.frames == (uint64_t) frames);
    TASK_CHECK(st.encoded_bits == bits);
    frame_list_free(&fl);
    free(out.data);
    return 0;
}

static int
test_ledger_repeated_flush(void)
{
    lame_abr_feedback_stats a, b;
    stream_t out;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    short   pcm[2 * 1152];
    rng_t   rl = { 0x12u }, rr = { 0x34u };
    int     i;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 60; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 8000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &a) == 0);

    /* a second flush must not settle the same frames again */
    {
        unsigned char tmp[8192];
        TASK_CHECK(lame_encode_flush(g, tmp, sizeof(tmp)) == 0);
    }
    TASK_CHECK(lame_get_abr_feedback_stats(g, &b) == 0);
    TASK_CHECK(a.frames == b.frames);
    TASK_CHECK(a.encoded_bits == b.encoded_bits);
    TASK_CHECK(a.budget_bit_samples == b.budget_bit_samples);

    /* the stream ends here, so targets are refused from now on */
    TASK_CHECK(lame_set_abr_target(g, 128) == -1);

    lame_close(g);
    free(out.data);
    return 0;
}

static int
test_ledger_target_budget(void)
{
    lame_abr_feedback_stats before, after;
    stream_t out;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    short   pcm[2 * 1152];
    rng_t   rl = { 0x56u }, rr = { 0x78u };
    int     i;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 40; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 8000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    /* budget already accrued belongs to the old target; from here on every
     * further frame must be budgeted at the new one */
    TASK_CHECK(lame_get_abr_feedback_stats(g, &before) == 0);
    TASK_CHECK(lame_set_abr_target(g, 160) == 0);

    for (i = 40; i < 80; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 8000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(lame_get_abr_feedback_stats(g, &after) == 0);

    TASK_CHECK(after.target_kbps == 160);
    TASK_CHECK(after.frames > before.frames);
    /* some frames emitted between the two snapshots were already queued at
     * the old target, so the budget must lie between the two extremes */
    {
        uint64_t const old_bits = 96ull * 1000ull * 1152ull;
        uint64_t const new_bits = 160ull * 1000ull * 1152ull;
        uint64_t const min_extra = (after.frames - before.frames) * old_bits;
        uint64_t const max_extra = (after.frames - before.frames) * new_bits;
        uint64_t const extra = after.budget_bit_samples - before.budget_bit_samples;
        TASK_CHECK(extra >= min_extra && extra <= max_extra);
        TASK_CHECK(after.budget_bit_samples - before.budget_bit_samples > 0);
    }
    lame_close(g);
    free(out.data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* group: feedback                                                    */
/* ------------------------------------------------------------------ */

#define SEGMENTS 3

static int
targeted_noise_run(int sr, int feedback, double avg[SEGMENTS], int distinct[SEGMENTS],
                   lame_abr_feedback_stats * final_stats, int seg_frames)
{
    static const int targets[SEGMENTS] = { 96, 160, 80 };
    stream_t out;
    short   pcm[2 * 1152];
    rng_t   rl = { 0x9E3779B9u }, rr = { 0x7F4A7C15u };
    lame_t  g = open_encoder(sr, targets[0], feedback, 0, 0);
    frame_list fl;
    int     i, seg, audio_frames;

    if (!g)
        return -1;
    memset(&out, 0, sizeof(out));

    for (seg = 0; seg < SEGMENTS; ++seg) {
        if (feedback) {
            if (lame_set_abr_target(g, targets[seg]) != 0) {
                lame_close(g);
                free(out.data);
                return -1;
            }
        }
        else if (seg > 0) {
            /* without feedback the target call is refused */
            if (lame_set_abr_target(g, targets[seg]) != -1) {
                lame_close(g);
                free(out.data);
                return -1;
            }
        }
        for (i = 0; i < seg_frames; ++i) {
            int const frame_index = seg * seg_frames + i;
            make_pcm(pcm, 1152, sr, SIG_NOISE, 11000, frame_index * 1152, &rl, &rr);
            if (encode_block(g, &out, pcm, 1152) != 0) {
                lame_close(g);
                free(out.data);
                return -1;
            }
        }
    }

    if (flush_stream(g, &out) != 0) {
        lame_close(g);
        free(out.data);
        return -1;
    }
    if (final_stats && lame_get_abr_feedback_stats(g, final_stats) != 0) {
        lame_close(g);
        free(out.data);
        return -1;
    }
    lame_close(g);

    if (parse_mp3(out.data, out.size, sr, &fl) != 0) {
        free(out.data);
        return -1;
    }
    audio_frames = fl.count - (fl.count ? fl.f[0].is_xing : 0);
    if (audio_frames < SEGMENTS * seg_frames) {
        frame_list_free(&fl);
        free(out.data);
        return -1;
    }

    /*
     * The encoder emits its first frame only after roughly one frame of
     * look-ahead; align the analysed segments with the frames produced by
     * the samples fed in.  Measuring the last 256 frames of every segment
     * is robust against that offset.
     */
    for (seg = 0; seg < SEGMENTS; ++seg) {
        int     end = (seg + 1) * seg_frames;
        int     start = end - 256;
        if (start < 0)
            start = 0;
        avg[seg] = run_kbps(&fl, start, end - start, sr);
        distinct[seg] = run_distinct(&fl, start, end - start);
    }

    frame_list_free(&fl);
    free(out.data);
    return 0;
}

static int
check_target_accuracy(void)
{
    static const int rates[2] = { 48000, 44100 };
    static const int targets[SEGMENTS] = { 96, 160, 80 };
    int     r;
    for (r = 0; r < 2; ++r) {
        int const sr = rates[r];
        double  avg[SEGMENTS];
        int     distinct[SEGMENTS];
        lame_abr_feedback_stats st;
        int     seg;

        memset(&st, 0, sizeof(st));
        TASK_CHECK(targeted_noise_run(sr, 1, avg, distinct, &st, 512) == 0);
        for (seg = 0; seg < SEGMENTS; ++seg) {
            double const want = targets[seg];
            double const got = avg[seg];
            double const lo = want * 0.95;
            double const hi = want * 1.05;
            if (!(got >= lo && got <= hi)) {
                fprintf(stderr,
                        "target %d kbps at %d Hz: measured %.2f kbps outside +/-5%%\n",
                        targets[seg], sr, got);
            }
            TASK_CHECK(got >= lo && got <= hi);
            /* complexity driven rate selection must remain, so the same
             * segment has to use more than one bitrate */
            TASK_CHECK(distinct[seg] >= 2);
        }
        TASK_CHECK(st.frames > 0);
        TASK_CHECK(st.target_kbps == 80);
    }
    return 0;
}

static int
test_feedback_disabled_keeps_stream(void)
{
    /* feedback off: target calls fail, stream stays a normal ABR stream */
    double  avg[1];
    frame_list fl;
    stream_t out;
    short   pcm[2 * 1152];
    rng_t   rl = { 0x2468u }, rr = { 0x1357u };
    lame_t  g = open_encoder(48000, 96, 0, 0, 0);
    lame_abr_feedback_stats st;
    int     i;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    TASK_CHECK(lame_set_abr_target(g, 160) == -1);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == -1);
    for (i = 0; i < 300; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 11000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    lame_close(g);

    TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);
    TASK_CHECK(run_distinct(&fl, 0, fl.count - 1) >= 2);
    /* plain ABR around 96, not forced to CBR */
    avg[0] = run_kbps(&fl, 0, fl.count - 1, 48000);
    TASK_CHECK(avg[0] > 70.0 && avg[0] < 120.0);
    frame_list_free(&fl);
    free(out.data);
    return 0;
}

static int
test_feedback_range_checks(void)
{
    lame_t  g = lame_init();
    lame_abr_feedback_stats st;

    TASK_CHECK(g != NULL);
    /* wrong argument before init */
    TASK_CHECK(lame_set_abr_feedback(g, 2) == -1);
    TASK_CHECK(lame_set_abr_feedback(g, -1) == -1);
    TASK_CHECK(lame_set_abr_feedback(g, 0) == 0);
    TASK_CHECK(lame_set_abr_feedback(g, 1) == 0);
    TASK_CHECK(lame_set_abr_target(g, 96) == -1);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == -1);
    TASK_CHECK(lame_get_abr_feedback_stats(g, NULL) == -1);

    lame_set_in_samplerate(g, 44100);
    lame_set_out_samplerate(g, 44100);
    lame_set_num_channels(g, 2);
    lame_set_VBR(g, vbr_abr);
    lame_set_VBR_mean_bitrate_kbps(g, 96);
    lame_set_VBR_min_bitrate_kbps(g, 64);
    lame_set_VBR_max_bitrate_kbps(g, 192);
    lame_set_bWriteVbrTag(g, 0);
    TASK_CHECK(lame_init_params(g) == 0);

    /* not settable once initialised */
    TASK_CHECK(lame_set_abr_feedback(g, 0) == -1);

    /* the effective range is the initialised VBR range */
    TASK_CHECK(lame_get_VBR_min_bitrate_kbps(g) == 64);
    TASK_CHECK(lame_get_VBR_max_bitrate_kbps(g) == 192);
    TASK_CHECK(lame_set_abr_target(g, 63) == -1);
    TASK_CHECK(lame_set_abr_target(g, 193) == -1);
    TASK_CHECK(lame_set_abr_target(g, 0) == -1);
    TASK_CHECK(lame_set_abr_target(g, -7) == -1);
    TASK_CHECK(lame_set_abr_target(g, 64) == 0);
    TASK_CHECK(lame_set_abr_target(g, 192) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
    TASK_CHECK(st.target_kbps == 96); /* not applied before a frame */

    lame_close(g);
    return 0;
}

static int
test_feedback_mode_rejected(void)
{
    static const vbr_mode modes[3] = { vbr_off, vbr_rh, vbr_mtrh };
    int     i;
    for (i = 0; i < 3; ++i) {
        lame_t  g = lame_init();
        TASK_CHECK(g != NULL);
        lame_set_in_samplerate(g, 44100);
        lame_set_out_samplerate(g, 44100);
        lame_set_num_channels(g, 2);
        lame_set_VBR(g, modes[i]);
        if (modes[i] == vbr_off)
            lame_set_brate(g, 128);
        lame_set_abr_feedback(g, 1);
        TASK_CHECK(lame_init_params(g) < 0);
        /* after a rejected initialisation the option can still be fixed */
        TASK_CHECK(lame_set_abr_feedback(g, 0) == 0);
        TASK_CHECK(lame_init_params(g) == 0);
        lame_close(g);
    }

    /* the same modes initialise fine with feedback off */
    for (i = 0; i < 3; ++i) {
        lame_t  g = lame_init();
        TASK_CHECK(g != NULL);
        lame_set_in_samplerate(g, 44100);
        lame_set_out_samplerate(g, 44100);
        lame_set_num_channels(g, 2);
        lame_set_VBR(g, modes[i]);
        if (modes[i] == vbr_off)
            lame_set_brate(g, 128);
        TASK_CHECK(lame_init_params(g) == 0);
        lame_close(g);
    }
    return 0;
}

static int
test_feedback_last_setting_wins(void)
{
    stream_t out;
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    short   pcm[2 * 1152];
    rng_t   rl = { 0xAAu }, rr = { 0xBBu };
    lame_abr_feedback_stats st;
    int     i;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 64; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 10000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    /* several settings before the next frame collapse to the last one */
    TASK_CHECK(lame_set_abr_target(g, 160) == 0);
    TASK_CHECK(lame_set_abr_target(g, 100) == 0);
    TASK_CHECK(lame_set_abr_target(g, 192) == 0);
    for (i = 64; i < 320; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 10000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
    TASK_CHECK(st.target_kbps == 192);
    lame_close(g);
    free(out.data);
    return 0;
}

static int
test_feedback_mixed_content(void)
{
    /* tone, transients and silence interleaved: must recover and stay
     * decodable; silence is not expected to reach the target */
    static const sig_kind kinds[6] =
        { SIG_TONE, SIG_TRANSIENT, SIG_SILENCE, SIG_TONE, SIG_SILENCE, SIG_TRANSIENT };
    static const int lens[6] = { 300, 400, 200, 400, 200, 300 };
    stream_t out;
    short   pcm[2 * 1152];
    rng_t   rl = { 0xDEADu }, rr = { 0xBEEFu };
    lame_t  g = open_encoder(48000, 96, 1, 0, 0);
    frame_list fl;
    int     seg, i, position = 0;
    int     audio_after_silence = 0;
    hip_t   hip;
    short   dl[1152], dr[1152];
    int     decoded = 0, nonzero = 0;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));

    for (seg = 0; seg < 6; ++seg) {
        for (i = 0; i < lens[seg]; ++i) {
            int     amp = kinds[seg] == SIG_TONE ? 22000 : 20000;
            make_pcm(pcm, 1152, 48000, kinds[seg], amp, position, &rl, &rr);
            position += 1152;
            TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
        }
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    lame_close(g);

    TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);

    /*
     * Tone segments are sized around the target, with room for the
     * natural variation caused by the neighbouring transients and the
     * reservoir.  The point is that the controller keeps the rate near
     * 96 kbps rather than drifting far away.
     */
    {
        double const tone = run_kbps(&fl, 300, 300, 48000);
        double const tone2 = run_kbps(&fl, 1100, 300, 48000);
        TASK_CHECK(tone > 70.0 && tone < 130.0);
        TASK_CHECK(tone2 > 60.0 && tone2 < 160.0);
        /* after the silent stretch the rate must come back up, not stay
         * stuck near the silence level */
        audio_after_silence = tone2 > 50.0;
        TASK_CHECK(audio_after_silence);
    }

    /* the silent stretch is allowed to fall below the target */
    {
        double const quiet = run_kbps(&fl, 700, 120, 48000);
        TASK_CHECK(quiet < 60.0);
    }

    /* the stream decodes continuously, in irregular chunks */
    hip = hip_decode_init();
    TASK_CHECK(hip != NULL);
    for (i = 0; i < out.size; i += 251) {
        int     len = out.size - i;
        int     got;
        if (len > 251)
            len = 251;
        got = hip_decode1(hip, out.data + i, len, dl, dr);
        if (got < 0) {
            hip_decode_exit(hip);
            frame_list_free(&fl);
            free(out.data);
            TASK_CHECK(0);
        }
        decoded += got;
        {
            int     k;
            for (k = 0; k < got; ++k)
                nonzero |= (dl[k] != 0 || dr[k] != 0);
        }
    }
    for (i = 0; i < 256; ++i) {
        int     got = hip_decode1(hip, out.data, 0, dl, dr);
        if (got <= 0)
            break;
        decoded += got;
    }
    hip_decode_exit(hip);
    TASK_CHECK(nonzero == 1);
    TASK_CHECK(decoded > 0);

    frame_list_free(&fl);
    free(out.data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* group: compatibility                                               */
/* ------------------------------------------------------------------ */

static int
test_compat_frame_syntax(void)
{
    static const int rates[2] = { 48000, 44100 };
    int     r, crc, xing;
    for (r = 0; r < 2; ++r) {
        for (crc = 0; crc <= 1; ++crc) {
            for (xing = 0; xing <= 1; ++xing) {
                int const sr = rates[r];
                stream_t out;
                frame_list fl;
                short   pcm[2 * 1152];
                rng_t   rl = { 0x11u }, rr = { 0x22u };
                lame_abr_feedback_stats st;
                int     i, frames;
                uint64_t bits;

                memset(&out, 0, sizeof(out));
                memset(&st, 0, sizeof(st));
                {
                    lame_t  g = open_encoder(sr, 128, 1, crc, xing);
                    TASK_CHECK(g != NULL);
                    for (i = 0; i < 150; ++i) {
                        make_pcm(pcm, 1152, sr, SIG_NOISE, 10000, i * 1152, &rl, &rr);
                        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
                    }
                    TASK_CHECK(flush_stream(g, &out) == 0);
                    if (xing)
                        TASK_CHECK(patch_xing_tag(g, &out) == 0);
                    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
                    lame_close(g);
                }
                /* strict, independent parse: every byte belongs to a frame */
                TASK_CHECK(parse_mp3(out.data, out.size, sr, &fl) == 0);
                for (i = 0; i < fl.count; ++i) {
                    TASK_CHECK(fl.f[i].channels == 2);
                    TASK_CHECK(fl.f[i].crc == crc);
                }
                /*
                 * The Xing placeholder is a physical frame too, but with the
                 * tag enabled it is excluded from the ledger.  Compare the
                 * ledger against the parser with the tag frame removed.
                 */
                {
                    int     n = 0;
                    uint64_t b = 0;
                    for (i = 0; i < fl.count; ++i) {
                        if (fl.f[i].is_xing)
                            continue;
                        ++n;
                        b += (uint64_t) fl.f[i].bytes * 8;
                    }
                    audio_totals(&fl, &frames, &bits);
                    TASK_CHECK(frames == n);
                    TASK_CHECK(bits == b);
                    TASK_CHECK(st.frames == (uint64_t) n);
                    TASK_CHECK(st.encoded_bits == b);
                }
                frame_list_free(&fl);
                free(out.data);
            }
        }
    }
    return 0;
}

static int
test_compat_chunking(void)
{
    /*
     * The same PCM fed as one big block or as small fragments must give
     * the same audio frames and the same ledger.
     */
    static const int chunks[3] = { 1152, 337, 1 };
    frame_list first;
    stream_t first_out;
    lame_abr_feedback_stats first_stats;
    int     c;

    memset(&first, 0, sizeof(first));
    memset(&first_out, 0, sizeof(first_out));
    memset(&first_stats, 0, sizeof(first_stats));

    for (c = 0; c < 3; ++c) {
        stream_t out;
        frame_list fl;
        lame_abr_feedback_stats st;
        lame_t  g = open_encoder(48000, 96, 1, 0, 0);
        short   pcm[2 * 1152];
        rng_t   rl = { 0x13579BDFu }, rr = { 0x2468ACE0u };
        int     position = 0, seg;

        TASK_CHECK(g != NULL);
        memset(&out, 0, sizeof(out));
        memset(&st, 0, sizeof(st));

        for (seg = 0; seg < 3; ++seg) {
            int     frame;
            static const int targets[3] = { 96, 160, 80 };
            TASK_CHECK(lame_set_abr_target(g, targets[seg]) == 0);
            for (frame = 0; frame < 400; ++frame) {
                int     done = 0;
                int     block_ok = 1;
                make_pcm(pcm, 1152, 48000, SIG_NOISE, 10000, position, &rl, &rr);
                position += 1152;
                while (done < 1152) {
                    int     n = 1152 - done;
                    unsigned char tmp[16384];
                    int     got;
                    if (n > chunks[c])
                        n = chunks[c];
                    got = lame_encode_buffer_interleaved(g, pcm + 2 * done, n, tmp, sizeof(tmp));
                    if (got < 0 || stream_append(&out, tmp, got) != 0)
                        block_ok = 0;
                    done += n;
                }
                TASK_CHECK(block_ok);
            }
        }
        TASK_CHECK(flush_stream(g, &out) == 0);
        TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
        lame_close(g);

        TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);

        if (c == 0) {
            first = fl;
            first_out = out;
            first_stats = st;
        }
        else {
            int     i;
            TASK_CHECK(fl.count == first.count);
            TASK_CHECK(out.size == first_out.size);
            TASK_CHECK(st.frames == first_stats.frames);
            TASK_CHECK(st.encoded_bits == first_stats.encoded_bits);
            TASK_CHECK(st.budget_bit_samples == first_stats.budget_bit_samples);
            TASK_CHECK(st.target_kbps == first_stats.target_kbps);
            if (out.size == first_out.size)
                TASK_CHECK(memcmp(out.data, first_out.data, (size_t) out.size) == 0);
            for (i = 0; i < fl.count && i < first.count; ++i) {
                TASK_CHECK(fl.f[i].bytes == first.f[i].bytes);
                TASK_CHECK(fl.f[i].kbps == first.f[i].kbps);
            }
            frame_list_free(&fl);
            free(out.data);
        }
        if (c == 0) {
            /* keep the reference until all runs are done */
        }
    }
    frame_list_free(&first);
    free(first_out.data);
    return 0;
}

static int
test_compat_decode(void)
{
    static const int rates[2] = { 48000, 44100 };
    int     r;
    for (r = 0; r < 2; ++r) {
        int const sr = rates[r];
        stream_t out;
        lame_abr_feedback_stats st;
        short   pcm[2 * 1152];
        rng_t   rl = { 0x555u }, rr = { 0x666u };
        int     i, decoded = 0, nonzero = 0;
        hip_t   hip;
        short   dl[1152], dr[1152];

        memset(&out, 0, sizeof(out));
        memset(&st, 0, sizeof(st));
        {
            /* no Xing placeholder here, so the decoded sample count is
             * exactly the ledger's frame count times 1152 */
            lame_t  g = open_encoder(sr, 128, 1, 0, 0);
            TASK_CHECK(g != NULL);
            for (i = 0; i < 200; ++i) {
                make_pcm(pcm, 1152, sr, SIG_TONE, 18000, i * 1152, &rl, &rr);
                TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
            }
            TASK_CHECK(flush_stream(g, &out) == 0);
            TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
            lame_close(g);
        }

        hip = hip_decode_init();
        TASK_CHECK(hip != NULL);
        for (i = 0; i < out.size; i += 199) {
            int     len = out.size - i;
            int     got;
            if (len > 199)
                len = 199;
            got = hip_decode1(hip, out.data + i, len, dl, dr);
            if (got < 0) {
                hip_decode_exit(hip);
                free(out.data);
                TASK_CHECK(0);
            }
            decoded += got;
            {
                int     k;
                for (k = 0; k < got; ++k)
                    nonzero |= (dl[k] != 0 || dr[k] != 0);
            }
        }
        for (i = 0; i < 256; ++i) {
            int     got = hip_decode1(hip, out.data, 0, dl, dr);
            if (got <= 0)
                break;
            decoded += got;
        }
        hip_decode_exit(hip);

        TASK_CHECK(nonzero == 1);
        /* all submitted frames produce 1152 samples each */
        TASK_CHECK((uint64_t) decoded == st.frames * 1152ull);

        free(out.data);
    }
    return 0;
}

static int
test_compat_gapless_identity(void)
{
    /* enabled feedback still uses the ordinary encoder core: an encoder
     * that never changes its target is driven only by the ledger */
    stream_t out;
    lame_abr_feedback_stats st;
    short   pcm[2 * 1152];
    rng_t   rl = { 0xCAFEu }, rr = { 0xF00Du };
    lame_t  g;
    int     i;

    memset(&out, 0, sizeof(out));
    g = open_encoder(48000, 96, 1, 0, 0);
    TASK_CHECK(g != NULL);
    for (i = 0; i < 100; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_NOISE, 9000, i * 1152, &rl, &rr);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
    /* the encoder delay and padding stay untouched by the feedback path */
    TASK_CHECK(lame_get_encoder_delay(g) == 576);
    TASK_CHECK(lame_get_encoder_padding(g) > 0);
    TASK_CHECK(st.frames * 1152ull >= 100ull * 1152ull);
    lame_close(g);
    free(out.data);
    return 0;
}

static int
test_compat_silence_not_padded(void)
{
    /* analog silence must not be stuffed up to the target */
    stream_t out;
    frame_list fl;
    short   pcm[2 * 1152];
    lame_t  g = open_encoder(48000, 128, 1, 0, 0);
    lame_abr_feedback_stats st;
    int     i;
    double  kbps;

    TASK_CHECK(g != NULL);
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 300; ++i) {
        make_pcm(pcm, 1152, 48000, SIG_SILENCE, 0, i * 1152, NULL, NULL);
        TASK_CHECK(encode_block(g, &out, pcm, 1152) == 0);
    }
    TASK_CHECK(flush_stream(g, &out) == 0);
    TASK_CHECK(lame_get_abr_feedback_stats(g, &st) == 0);
    lame_close(g);

    TASK_CHECK(parse_mp3(out.data, out.size, 48000, &fl) == 0);
    kbps = run_kbps(&fl, 0, fl.count - (fl.f[0].is_xing ? 1 : 0), 48000);
    TASK_CHECK(kbps < 48.0);
    frame_list_free(&fl);
    free(out.data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "ledger", "empty", test_ledger_empty },
    { "ledger", "no_output_calls", test_ledger_no_output_calls },
    { "ledger", "flush_frames", test_ledger_flush_frames },
    { "ledger", "signed_error", test_ledger_signed_error },
    { "ledger", "matches_parser", test_ledger_matches_parser },
    { "ledger", "excludes_xing", test_ledger_excludes_xing },
    { "ledger", "crc", test_ledger_crc },
    { "ledger", "repeated_flush", test_ledger_repeated_flush },
    { "ledger", "target_budget", test_ledger_target_budget },

    { "feedback", "target_accuracy", check_target_accuracy },
    { "feedback", "disabled_keeps_stream", test_feedback_disabled_keeps_stream },
    { "feedback", "range_checks", test_feedback_range_checks },
    { "feedback", "mode_rejected", test_feedback_mode_rejected },
    { "feedback", "last_setting_wins", test_feedback_last_setting_wins },
    { "feedback", "mixed_content", test_feedback_mixed_content },

    { "compatibility", "frame_syntax", test_compat_frame_syntax },
    { "compatibility", "chunking", test_compat_chunking },
    { "compatibility", "decode", test_compat_decode },
    { "compatibility", "gapless_identity", test_compat_gapless_identity },
    { "compatibility", "silence_not_padded", test_compat_silence_not_padded }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
