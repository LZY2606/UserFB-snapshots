/*
 * Tests for the LAME ancillary tail slot plan.
 *
 * The cases are registered in the three groups required by the task
 * harness: placement, reservoir and streaming.  Every MP3 stream is
 * parsed independently from its side information and the physical
 * main-data regions are numbered on their own byte axis, so the slot
 * guarantees are checked against the real bitstream rather than the
 * encoder's internal bookkeeping.
 */

#include "lame.h"
#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SAMPLES    (90 * 1152 + 137)
#define TEST_CAP        (2 * 1024 * 1024)
#define MAX_FRAMES      512
#define MAX_SLOT_BYTES  32

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static unsigned
read_bits(const unsigned char *p, int at, int n)
{
    unsigned v = 0;
    int j;
    for (j = 0; j < n; ++j)
        v = (unsigned) ((v << 1) | ((p[(at + j) / 8] >> (7 - (at + j) % 8)) & 1));
    return v;
}

static const int test_bitrate_table[2][16] = {
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}
};
static const int test_samplerate_table[3] = { 44100, 48000, 32000 };

struct parsed_frame {
    int off;        /* offset inside the stream                 */
    int len;        /* physical frame length in bytes           */
    int side;       /* side information length, without CRC     */
    int crc;        /* 2 when error protection is on            */
    int capacity;   /* physical main-data bytes                 */
    int mdb;        /* main_data_begin, B_i                     */
    int p23;        /* sum of part2_3_length, P_i (bits)        */
    int padding;
    int mpeg1;
    int channels;
    long c;         /* C_i: cumulative main-data byte axis      */
    int is_tag;     /* Xing/Info frame, not an audio frame      */
};

/* The initial Xing/Info frame is emitted as an all-zero placeholder when
 * the application never seeks back to fill the final tag.  Its whole
 * header payload (CRC position plus side information) is zero, whereas a
 * real silent audio frame still carries non-zero global gain fields. */
static int
tag_placeholder(const unsigned char *h, int side, int crc)
{
    int i;
    for (i = 4; i < 4 + crc + side; ++i)
        if (h[i] != 0)
            return 0;
    return 1;
}

/* Parse every MP3 frame.  Leading ID3v2 and trailing ID3v1 chunks are
 * skipped, and Xing/Info frames are flagged but still returned so the
 * caller can verify they are excluded from audio frame numbering. */
static int
parse_stream(const unsigned char *data, int size, struct parsed_frame *out, int max_out)
{
    int off = 0, n = 0;
    long c = 0;

    /* ID3v2 header */
    if (size > 10 && memcmp(data, "ID3", 3) == 0) {
        int id3size = 10 + ((data[6] & 0x7f) << 21) + ((data[7] & 0x7f) << 14)
                      + ((data[8] & 0x7f) << 7) + (data[9] & 0x7f);
        off += id3size;
    }

    while (off + 4 <= size) {
        const unsigned char *h = data + off;
        int mpeg1, ver, rate, ch, crc, gr, j, at, len, p = 0;
        const unsigned char *s;

        if (h[0] == 'T' && h[1] == 'A' && h[2] == 'G')
            break;  /* trailing ID3v1 */
        if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0)
            return -1;
        ver = (h[1] >> 3) & 3;
        mpeg1 = (ver == 3);
        if (ver != 3 && ver != 2)
            return -1;
        rate = test_samplerate_table[(h[2] >> 2) & 3] / (mpeg1 ? 1 : 2);
        ch = (h[3] >> 6) == 3 ? 1 : 2;
        crc = (h[1] & 1) ? 0 : 2;
        len = (mpeg1 ? 144000 : 72000) * test_bitrate_table[mpeg1][h[2] >> 4] / rate
              + ((h[2] >> 1) & 1);
        if (len <= 0 || off + len > size || n >= max_out)
            return -1;

        out[n].off = off;
        out[n].len = len;
        out[n].side = mpeg1 ? (ch == 1 ? 17 : 32) : (ch == 1 ? 9 : 17);
        out[n].crc = crc;
        out[n].capacity = len - 4 - crc - out[n].side;
        out[n].padding = (h[2] >> 1) & 1;
        out[n].mpeg1 = mpeg1;
        out[n].channels = ch;
        out[n].c = c;
        out[n].is_tag = tag_placeholder(h, out[n].side, crc);

        s = h + 4 + crc;
        out[n].mdb = (int) read_bits(s, 0, mpeg1 ? 9 : 8);
        at = mpeg1 ? (9 + (ch == 1 ? 5 : 3) + 4 * ch) : (8 + (ch == 1 ? 1 : 2));
        for (gr = 0; gr < (mpeg1 ? 2 : 1); ++gr)
            for (j = 0; j < ch; ++j) {
                p += (int) read_bits(s, at, 12);
                at += mpeg1 ? 59 : 63;
            }
        out[n].p23 = p;

        if (!out[n].is_tag)
            c += out[n].capacity;
        ++n;
        off += len;
    }
    return n;
}

/* audio frames are parsed frames with the Xing/Info ones removed */
static int
audio_frames(const struct parsed_frame *all, int nall,
             struct parsed_frame *audio, int max_audio)
{
    int i, k = 0;
    for (i = 0; i < nall; ++i) {
        if (all[i].is_tag)
            continue;
        if (k >= max_audio)
            return -1;
        audio[k++] = all[i];
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* signal generation                                                  */
/* ------------------------------------------------------------------ */

static void
gen_silence(short *l, short *r, int n)
{
    memset(l, 0, (size_t) n * sizeof(short));
    memset(r, 0, (size_t) n * sizeof(short));
}

static void
gen_noise(short *l, short *r, int n, uint32_t seed)
{
    uint32_t rng = seed;
    int j;
    for (j = 0; j < n; ++j) {
        rng = 1664525u * rng + 1013904223u;
        l[j] = (short) ((rng >> 16) - 32768);
        rng = 1664525u * rng + 1013904223u;
        r[j] = (short) ((rng >> 16) - 32768);
    }
}

/* 40 frame pattern: 8 silence, 24 independent noise, 8 silence, with
 * sharp transients at the noise entry, mirroring the baseline stream */
static void
gen_pattern(short *l, short *r, int n)
{
    uint32_t rng = 0x12345678u;
    int j;
    gen_silence(l, r, n);
    for (j = 0; j < n; ++j) {
        int phase = (j / 1152) % 40;
        if (phase >= 8 && phase < 32) {
            rng = 1664525u * rng + 1013904223u;
            l[j] = (short) ((rng >> 18) - 8192);
            rng = 1664525u * rng + 1013904223u;
            r[j] = (short) ((rng >> 18) - 8192);
        }
        if (phase == 8 && (j % 1152) < 64) {   /* attack transient */
            rng = 1664525u * rng + 1013904223u;
            l[j] = (short) ((rng >> 16) - 32768);
            rng = 1664525u * rng + 1013904223u;
            r[j] = (short) ((rng >> 16) - 32768);
        }
    }
}

/* Long silent stretch (45 frames, well above the required 40), followed
 * by independent dual channel noise opened by a sharp transient, then
 * silence again.  Used to observe real reservoir accumulation and the
 * later borrowing into the noisy frames. */
static void
gen_reservoir_signal(short *l, short *r, int n)
{
    uint32_t rl = 2654435761u, rr = 40503u;
    int j;
    gen_silence(l, r, n);
    for (j = 45 * 1152; j < n && j < 75 * 1152; ++j) {
        rl = 1664525u * rl + 1013904223u;
        rr = 1664525u * rr + 1013904223u;
        l[j] = (short) ((rl >> 17) - 16384);
        r[j] = (short) ((rr >> 17) - 16384);
    }
    /* attack transient at the noise entry */
    for (j = 45 * 1152; j < n && j < 45 * 1152 + 128; ++j) {
        rl = 1664525u * rl + 1013904223u;
        rr = 1664525u * rr + 1013904223u;
        l[j] = (short) ((rl >> 16) - 32768);
        r[j] = (short) ((rr >> 16) - 32768);
    }
}

/* ------------------------------------------------------------------ */
/* encoder helpers                                                    */
/* ------------------------------------------------------------------ */

struct enc_cfg {
    int rate;
    int channels;
    MPEG_mode mode;
    int brate;
    int crc;
    int tag;
};

static void
cfg_encoder(lame_t g, const struct enc_cfg *cfg)
{
    lame_set_in_samplerate(g, cfg->rate);
    lame_set_out_samplerate(g, cfg->rate);
    lame_set_num_channels(g, cfg->channels);
    lame_set_mode(g, cfg->mode);
    lame_set_brate(g, cfg->brate);
    lame_set_quality(g, 2);
    lame_set_VBR(g, vbr_off);
    lame_set_error_protection(g, cfg->crc);
    lame_set_bWriteVbrTag(g, cfg->tag);
    lame_set_write_id3tag_automatic(g, cfg->tag);
}

static int
encode_planar(const struct enc_cfg *cfg, const lame_ancillary_slot *slots,
              size_t nslot, int chunked, const short *l, const short *r,
              int nsamples, unsigned char *out)
{
    lame_t g = lame_init();
    int used = 0, j, k;
    cfg_encoder(g, cfg);
    if (slots != NULL)
        TASK_CHECK(lame_set_ancillary_slots(g, slots, nslot) == 0);
    TASK_CHECK(lame_init_params(g) == 0);
    if (chunked) {
        for (j = 0; j < nsamples;) {
            int len = 1 + (j % 2049);
            if (len > nsamples - j)
                len = nsamples - j;
            k = lame_encode_buffer(g, l + j, r + j, len, out + used, TEST_CAP - used);
            TASK_CHECK(k >= 0);
            used += k;
            j += len;
        }
    }
    else {
        k = lame_encode_buffer(g, l, r, nsamples, out, TEST_CAP);
        TASK_CHECK(k >= 0);
        used = k;
    }
    k = lame_encode_flush(g, out + used, TEST_CAP - used);
    TASK_CHECK(k >= 0);
    used += k;
    TASK_CHECK(lame_encode_flush(g, out + used, TEST_CAP - used) == 0);
    lame_close(g);
    return used;
}

static int
encode_interleaved(const struct enc_cfg *cfg, const lame_ancillary_slot *slots,
                   size_t nslot, int chunked, const short *l, const short *r,
                   int nsamples, unsigned char *out)
{
    lame_t g = lame_init();
    short *interleaved = malloc((size_t) 2 * nsamples * sizeof(short));
    int used = 0, j, k;
    for (j = 0; j < nsamples; ++j) {
        interleaved[2 * j] = l[j];
        interleaved[2 * j + 1] = r[j];
    }
    cfg_encoder(g, cfg);
    if (slots != NULL)
        TASK_CHECK(lame_set_ancillary_slots(g, slots, nslot) == 0);
    TASK_CHECK(lame_init_params(g) == 0);
    if (chunked) {
        for (j = 0; j < nsamples;) {
            int len = 1 + (j % 1777);
            if (len > nsamples - j)
                len = nsamples - j;
            k = lame_encode_buffer_interleaved(g, interleaved + 2 * j, len,
                                               out + used, TEST_CAP - used);
            TASK_CHECK(k >= 0);
            used += k;
            j += len;
        }
    }
    else {
        k = lame_encode_buffer_interleaved(g, interleaved, nsamples, out, TEST_CAP);
        TASK_CHECK(k >= 0);
        used = k;
    }
    k = lame_encode_flush(g, out + used, TEST_CAP - used);
    TASK_CHECK(k >= 0);
    used += k;
    free(interleaved);
    lame_close(g);
    return used;
}

/* ------------------------------------------------------------------ */
/* decoding                                                           */
/* ------------------------------------------------------------------ */

struct decode_result {
    long samples;
    long nonzero;
    unsigned long hash;
};

static void
quiet_report(const char *fmt, va_list ap)
{
    (void) fmt;
    (void) ap;
}

static int
decode_stream(const unsigned char *data, int size, struct decode_result *res)
{
    hip_t h = hip_decode_init();
    short pl[1152], pr[1152];
    int off = 0, n = 0, guard = 0;
    unsigned long hash = 2166136261u;
    long total = 0, nonzero = 0;
    TASK_CHECK(h != NULL);
    hip_set_errorf(h, quiet_report);
    hip_set_debugf(h, quiet_report);
    hip_set_msgf(h, quiet_report);
    do {
        int len = off < size ? (size - off > 73 ? 73 : size - off) : 0;
        int i;
        n = hip_decode1(h, (unsigned char *) data + off, (size_t) len, pl, pr);
        off += len;
        if (n < 0) {
            hip_decode_exit(h);
            return -1;
        }
        for (i = 0; i < n; ++i) {
            hash = (hash ^ (unsigned short) pl[i]) * 16777619u;
            hash = (hash ^ (unsigned short) pr[i]) * 16777619u;
            if (pl[i] || pr[i])
                ++nonzero;
        }
        total += n;
        TASK_CHECK(++guard < 1000000);
    } while (off < size || n > 0);
    hip_decode_exit(h);
    res->samples = total;
    res->nonzero = nonzero;
    res->hash = hash;
    return 0;
}

/* CRC16 used by the MPEG header, polynomial 0x8005 */
#define CRC_POLY 0x8005

static int
crc_update(int value, int crc)
{
    int i;
    value <<= 8;
    for (i = 0; i < 8; ++i) {
        value <<= 1;
        crc <<= 1;
        if (((crc ^ value) & 0x10000))
            crc ^= CRC_POLY;
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* stream wide verification of the slot/audio geometry                */
/* ------------------------------------------------------------------ */

struct slot_expect {
    unsigned long frame;
    int size;
    const unsigned char *data;
};

/* Verify, using only the parsed side information:
 *  - every slot occupies exactly the physical frame tail,
 *  - every audio interval is disjoint from every slot,
 *  - audio intervals are mutually ordered/non-overlapping and stay in
 *    the physical frame,
 *  - main_data_begin back pointers are self consistent. */
static int
verify_geometry(const unsigned char *data,
                const struct parsed_frame *audio, int naudio,
                const struct slot_expect *slots, int nslots)
{
    int i, si;

    for (i = 0; i < naudio; ++i) {
        long a = 8 * (audio[i].c - audio[i].mdb);
        long b = a + audio[i].p23;
        long end = 8 * (audio[i].c + audio[i].capacity);
        TASK_CHECK(audio[i].c >= audio[i].mdb);
        TASK_CHECK(b <= end);
        if (i > 0) {
            long pa = 8 * (audio[i - 1].c - audio[i - 1].mdb);
            long pb = pa + audio[i - 1].p23;
            TASK_CHECK(a >= pb);
        }
    }

    for (si = 0; si < nslots; ++si) {
        int f = (int) slots[si].frame;
        int n = slots[si].size;
        long sa, sb;
        TASK_CHECK(f >= 0 && f < naudio);
        TASK_CHECK(n >= 1 && n <= MAX_SLOT_BYTES);
        TASK_CHECK(memcmp(data + audio[f].off + audio[f].len - n,
                          slots[si].data, (size_t) n) == 0);
        sa = 8 * (audio[f].c + audio[f].capacity - n);
        sb = 8 * (audio[f].c + audio[f].capacity);
        for (i = 0; i < naudio; ++i) {
            long a = 8 * (audio[i].c - audio[i].mdb);
            long b = a + audio[i].p23;
            TASK_CHECK(!(a < sb && b > sa));
        }
    }
    return 0;
}

static void
fill_slot_data(unsigned char *buf, int n, unsigned char seed)
{
    int j;
    for (j = 0; j < n; ++j)
        buf[j] = (unsigned char) (seed + 17 * j + 3 * j * j);
}

/* ================================================================== */
/* placement group                                                    */
/* ================================================================== */

static int
case_placement_tail_bytes(void)
{
    static unsigned char d0[4], d1[32], d2[1];
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    struct slot_expect expect[3];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[3];
    int nall, naudio, used, last;

    fill_slot_data(d0, 4, 11);
    fill_slot_data(d1, 32, 23);
    fill_slot_data(d2, 1, 99);
    gen_pattern(l, r, TEST_SAMPLES);

    slots[0].audio_frame = 0;  slots[0].data = d0; slots[0].size = 4;
    slots[1].audio_frame = 5;  slots[1].data = d1; slots[1].size = 32;
    slots[2].audio_frame = 10; slots[2].data = d2; slots[2].size = 1;

    used = encode_planar(&cfg, slots, 3, 1, l, r, TEST_SAMPLES, out);
    TASK_CHECK(used > 0);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    TASK_CHECK(nall > 11);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio == nall);   /* no tags in this configuration */

    expect[0].frame = 0;  expect[0].size = 4;  expect[0].data = d0;
    expect[1].frame = 5;  expect[1].size = 32; expect[1].data = d1;
    expect[2].frame = 10; expect[2].size = 1;  expect[2].data = d2;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 3) == 0);

    /* first frame and a mid stream frame really carry the bytes */
    TASK_CHECK(memcmp(out + audio[0].off + audio[0].len - 4, d0, 4) == 0);
    TASK_CHECK(memcmp(out + audio[5].off + audio[5].len - 32, d1, 32) == 0);

    /* frame length/bitrate follow the ordinary rules: both 44.1k sizes */
    last = naudio - 1;
    TASK_CHECK(audio[last].len == 417 || audio[last].len == 418);
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_placement_crc_padding(void)
{
    static unsigned char d0[32], d1[20];
    struct enc_cfg cfg = { 44100, 2, JOINT_STEREO, 128, 1, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    struct slot_expect expect[2];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[2];
    int nall, naudio, used, i, saw417 = 0, saw418 = 0;

    fill_slot_data(d0, 32, 5);
    fill_slot_data(d1, 20, 50);
    gen_pattern(l, r, TEST_SAMPLES);
    slots[0].audio_frame = 2; slots[0].data = d0; slots[0].size = 32;
    slots[1].audio_frame = 12; slots[1].data = d1; slots[1].size = 20;

    used = encode_planar(&cfg, slots, 2, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    TASK_CHECK(nall > 13);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio == nall);

    for (i = 0; i < naudio; ++i) {
        saw417 |= (audio[i].len == 417);
        saw418 |= (audio[i].len == 418);
        TASK_CHECK(audio[i].crc == 2);
        /* protection bit clear */
        TASK_CHECK((out[audio[i].off + 1] & 1) == 0);
        /* recompute header CRC: bytes 2,3 then side info 6..sideinfo_len */
        {
            int crc = 0xffff, k;
            crc = crc_update(out[audio[i].off + 2], crc);
            crc = crc_update(out[audio[i].off + 3], crc);
            for (k = 6; k < audio[i].side + 6; ++k)
                crc = crc_update(out[audio[i].off + k], crc);
            crc &= 0xffff;
            TASK_CHECK((crc >> 8) == out[audio[i].off + 4]);
            TASK_CHECK((crc & 255) == out[audio[i].off + 5]);
        }
    }
    TASK_CHECK(saw417 && saw418);   /* both physical frame lengths */

    expect[0].frame = 2; expect[0].size = 32; expect[0].data = d0;
    expect[1].frame = 12; expect[1].size = 20; expect[1].data = d1;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 2) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_placement_xing_id3(void)
{
    static unsigned char d0[16], d1[8];
    struct enc_cfg cfg = { 44100, 2, JOINT_STEREO, 128, 0, 1 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    struct slot_expect expect[2];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[2];
    int nall, naudio, used, i, tags = 0;

    fill_slot_data(d0, 16, 7);
    fill_slot_data(d1, 8, 71);
    gen_pattern(l, r, TEST_SAMPLES);
    slots[0].audio_frame = 0; slots[0].data = d0; slots[0].size = 16;
    slots[1].audio_frame = 4; slots[1].data = d1; slots[1].size = 8;

    /* encode directly so an ID3v2 title tag is really emitted */
    {
        lame_t g = lame_init();
        int j, k;
        cfg_encoder(g, &cfg);
        id3tag_init(g);
        id3tag_add_v2(g);
        id3tag_set_title(g, "ancillary");
        TASK_CHECK(lame_set_ancillary_slots(g, slots, 2) == 0);
        TASK_CHECK(lame_init_params(g) == 0);
        used = 0;
        for (j = 0; j < TEST_SAMPLES;) {
            int len = 1 + (j % 2049);
            if (len > TEST_SAMPLES - j)
                len = TEST_SAMPLES - j;
            k = lame_encode_buffer(g, l + j, r + j, len, out + used, TEST_CAP - used);
            TASK_CHECK(k >= 0);
            used += k;
            j += len;
        }
        k = lame_encode_flush(g, out + used, TEST_CAP - used);
        TASK_CHECK(k >= 0);
        used += k;
        lame_close(g);
    }
    TASK_CHECK(used > 0);
    TASK_CHECK(memcmp(out, "ID3", 3) == 0);   /* ID3v2 up front */
    nall = parse_stream(out, used, all, MAX_FRAMES);
    TASK_CHECK(nall > 5);
    for (i = 0; i < nall; ++i)
        tags += all[i].is_tag;
    TASK_CHECK(tags >= 1);   /* Xing/Info frame present */
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio == nall - tags);

    /* slot 0 lands on the first AUDIO frame, after the tag frame */
    expect[0].frame = 0; expect[0].size = 16; expect[0].data = d0;
    expect[1].frame = 4; expect[1].size = 8;  expect[1].data = d1;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 2) == 0);
    TASK_CHECK(memcmp(out + audio[0].off + audio[0].len - 16, d0, 16) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_placement_decode_invariant(void)
{
    static unsigned char d0[32], d1[32];
    struct enc_cfg cfg = { 48000, 2, STEREO, 128, 1, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    unsigned char *out = malloc(TEST_CAP);
    unsigned char *changed = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[2];
    struct decode_result a, b;
    int nall, naudio, used;

    fill_slot_data(d0, 32, 3);
    fill_slot_data(d1, 32, 4);
    gen_noise(l, r, TEST_SAMPLES, 777);
    slots[0].audio_frame = 1; slots[0].data = d0; slots[0].size = 32;
    slots[1].audio_frame = 9; slots[1].data = d1; slots[1].size = 32;

    used = encode_planar(&cfg, slots, 2, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio > 10);

    TASK_CHECK(decode_stream(out, used, &a) == 0);
    TASK_CHECK(a.nonzero > 0);
    TASK_CHECK(a.samples == (long) naudio * 1152);

    /* alter only the slot bytes; PCM must be identical sample for sample */
    memcpy(changed, out, (size_t) used);
    memset(changed + audio[1].off + audio[1].len - 32, 0xa5, 32);
    memset(changed + audio[9].off + audio[9].len - 32, 0x5a, 32);
    TASK_CHECK(decode_stream(changed, used, &b) == 0);
    TASK_CHECK(b.samples == a.samples);
    TASK_CHECK(b.nonzero == a.nonzero);
    TASK_CHECK(b.hash == a.hash);
    free(out);
    free(changed);
    free(l);
    free(r);
    return 0;
}

static int
case_placement_config_support(void)
{
    static unsigned char d[12];
    struct enc_cfg good[6] = {
        {24000, 1, MONO, 64, 0, 0},
        {44100, 1, MONO, 64, 0, 0},
        {48000, 1, MONO, 96, 1, 0},
        {44100, 2, STEREO, 128, 0, 0},
        {44100, 2, JOINT_STEREO, 160, 1, 0},
        {48000, 2, JOINT_STEREO, 64, 0, 0},
    };
    short *l = calloc(2048, sizeof(short));
    short *r = calloc(2048, sizeof(short));
    unsigned char *out = malloc(TEST_CAP);
    lame_ancillary_slot slot;
    int i;
    fill_slot_data(d, 12, 1);
    gen_noise(l, r, 2048, 11);
    slot.audio_frame = 0;
    slot.data = d;
    slot.size = 12;

    for (i = 0; i < 6; ++i) {
        lame_t g = lame_init();
        cfg_encoder(g, &good[i]);
        TASK_CHECK(lame_set_ancillary_slots(g, &slot, 1) == 0);
        TASK_CHECK(lame_init_params(g) == 0);
        TASK_CHECK(lame_encode_buffer(g, l, r, 2048, out, TEST_CAP) >= 0);
        TASK_CHECK(lame_encode_flush(g, out, TEST_CAP) >= 0);
        lame_close(g);
    }

    /* unsupported configurations must be rejected at init_params */
    {
        struct enc_cfg bad[4] = {
            {32000, 2, STEREO, 128, 0, 0},     /* unsupported sample rate */
            {44100, 2, STEREO, 56, 0, 0},      /* bitrate below 64 */
            {44100, 2, STEREO, 192, 0, 0},     /* bitrate above 160 */
            {22050, 1, MONO, 64, 0, 0},        /* unsupported sample rate */
        };
        for (i = 0; i < 4; ++i) {
            lame_t g = lame_init();
            cfg_encoder(g, &bad[i]);
            TASK_CHECK(lame_set_ancillary_slots(g, &slot, 1) == 0);
            TASK_CHECK(lame_init_params(g) == -1);
            lame_close(g);
        }
    }
    /* resampling rejected */
    {
        lame_t g = lame_init();
        lame_set_in_samplerate(g, 48000);
        lame_set_out_samplerate(g, 44100);
        lame_set_num_channels(g, 2);
        lame_set_mode(g, STEREO);
        lame_set_brate(g, 128);
        lame_set_VBR(g, vbr_off);
        TASK_CHECK(lame_set_ancillary_slots(g, &slot, 1) == 0);
        TASK_CHECK(lame_init_params(g) == -1);
        lame_close(g);
    }
    free(out);
    free(l);
    free(r);
    return 0;
}

/* ================================================================== */
/* reservoir group                                                    */
/* ================================================================== */

static int
case_reservoir_real_borrowing(void)
{
    /* No slots at all in the middle region: observe genuine reservoir
     * behavior, non-zero back pointers and a frame exceeding its own
     * physical main-data capacity. */
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    int nall, naudio, used, i, saw_backptr = 0, saw_over_own = 0;
    int silent_run = 0, max_silent_run = 0;

    gen_reservoir_signal(l, r, TEST_SAMPLES);
    used = encode_planar(&cfg, NULL, 0, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio >= 60);

    for (i = 0; i < naudio; ++i) {
        if (audio[i].mdb > 0)
            saw_backptr = 1;
        if (audio[i].p23 > 8 * audio[i].capacity)
            saw_over_own = 1;
        if (audio[i].p23 == 0) {
            ++silent_run;
            if (silent_run > max_silent_run)
                max_silent_run = silent_run;
        }
        else
            silent_run = 0;
    }
    TASK_CHECK(saw_backptr);
    TASK_CHECK(saw_over_own);
    TASK_CHECK(max_silent_run >= 40);   /* at least 40 silent frames */
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_reservoir_drain_and_rebuild(void)
{
    static unsigned char d0[32], d1[32];
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 1, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    struct slot_expect expect[2];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[2];
    int nall, naudio, used, i, rebuilt = 0;

    fill_slot_data(d0, 32, 13);
    fill_slot_data(d1, 32, 31);
    gen_pattern(l, r, TEST_SAMPLES);
    /* first slot after the reservoir has filled, second much later */
    slots[0].audio_frame = 9;  slots[0].data = d0; slots[0].size = 32;
    slots[1].audio_frame = 50; slots[1].data = d1; slots[1].size = 32;

    used = encode_planar(&cfg, slots, 2, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio > 52);

    /* the frame following each slotted frame starts with empty reservoir */
    TASK_CHECK(audio[10].mdb == 0);
    TASK_CHECK(audio[51].mdb == 0);

    /* slotted frames may still use previously accumulated reserve */
    TASK_CHECK(audio[9].p23 > 0);

    /* reservoir rebuilds in the slot-free region after the first drain */
    for (i = 11; i < 50; ++i)
        if (audio[i].mdb > 0)
            rebuilt = 1;
    TASK_CHECK(rebuilt);

    expect[0].frame = 9; expect[0].size = 32; expect[0].data = d0;
    expect[1].frame = 50; expect[1].size = 32; expect[1].data = d1;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 2) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_reservoir_slot_in_noisy_region(void)
{
    /* Put a slot directly on frames that, without slots, would borrow
     * heavily: the quantizer must fit without touching the tail, and the
     * next frame must still start at main_data_begin 0. */
    static unsigned char d[32];
    struct enc_cfg cfg = { 44100, 2, STEREO, 64, 0, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    struct slot_expect expect[3];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[3];
    int nall, naudio, used, i;

    fill_slot_data(d, 32, 64);
    gen_noise(l, r, TEST_SAMPLES, 555);
    slots[0].audio_frame = 5;  slots[0].data = d; slots[0].size = 32;
    slots[1].audio_frame = 6;  slots[1].data = d; slots[1].size = 32;
    slots[2].audio_frame = 7;  slots[2].data = d; slots[2].size = 32;

    used = encode_planar(&cfg, slots, 3, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio > 8);

    for (i = 0; i < 3; ++i) {
        int f = 5 + i;
        long a = 8 * (audio[f].c - audio[f].mdb);
        long b = a + audio[f].p23;
        long slot_start = 8 * (audio[f].c + audio[f].capacity - 32);
        TASK_CHECK(b <= slot_start);
    }
    TASK_CHECK(audio[8].mdb == 0);

    expect[0].frame = 5; expect[0].size = 32; expect[0].data = d;
    expect[1].frame = 6; expect[1].size = 32; expect[1].data = d;
    expect[2].frame = 7; expect[2].size = 32; expect[2].data = d;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 3) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

/* ================================================================== */
/* streaming group                                                    */
/* ================================================================== */

static int
case_streaming_chunk_equivalence(void)
{
    static unsigned char d0[24], d1[10];
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };
    unsigned char *a = malloc(TEST_CAP), *b = malloc(TEST_CAP), *c = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[2];
    int ua, ub, uc;

    fill_slot_data(d0, 24, 9);
    fill_slot_data(d1, 10, 90);
    gen_pattern(l, r, TEST_SAMPLES);
    slots[0].audio_frame = 3; slots[0].data = d0; slots[0].size = 24;
    slots[1].audio_frame = 20; slots[1].data = d1; slots[1].size = 10;

    ua = encode_planar(&cfg, slots, 2, 0, l, r, TEST_SAMPLES, a);       /* one block */
    ub = encode_planar(&cfg, slots, 2, 1, l, r, TEST_SAMPLES, b);       /* fragmented planar */
    uc = encode_interleaved(&cfg, slots, 2, 1, l, r, TEST_SAMPLES, c);  /* fragmented interleaved */
    TASK_CHECK(ua > 0 && ua == ub && ub == uc);
    TASK_CHECK(memcmp(a, b, (size_t) ua) == 0);
    TASK_CHECK(memcmp(a, c, (size_t) ua) == 0);
    free(a);
    free(b);
    free(c);
    free(l);
    free(r);
    return 0;
}

static int
case_streaming_dual_instance(void)
{
    static unsigned char d1[12], d2[7];
    struct enc_cfg cfg = { 48000, 2, JOINT_STEREO, 128, 0, 0 };
    unsigned char *a = malloc(TEST_CAP), *b = malloc(TEST_CAP);
    unsigned char *ra = malloc(TEST_CAP), *rb = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot s1 = { 0, d1, 12 };
    lame_ancillary_slot s2 = { 6, d2, 7 };
    lame_t g1, g2;
    int u1 = 0, u2 = 0, j, k;

    fill_slot_data(d1, 12, 2);
    fill_slot_data(d2, 7, 4);
    gen_noise(l, r, TEST_SAMPLES, 31337);

    g1 = lame_init();
    g2 = lame_init();
    cfg_encoder(g1, &cfg);
    cfg_encoder(g2, &cfg);
    TASK_CHECK(lame_set_ancillary_slots(g1, &s1, 1) == 0);
    TASK_CHECK(lame_set_ancillary_slots(g2, &s2, 1) == 0);
    TASK_CHECK(lame_init_params(g1) == 0);
    TASK_CHECK(lame_init_params(g2) == 0);

    for (j = 0; j < TEST_SAMPLES;) {
        int len = 1 + (j % 977);
        if (len > TEST_SAMPLES - j)
            len = TEST_SAMPLES - j;
        k = lame_encode_buffer(g1, l + j, r + j, len, a + u1, TEST_CAP - u1);
        TASK_CHECK(k >= 0);
        u1 += k;
        k = lame_encode_buffer(g2, l + j, r + j, len, b + u2, TEST_CAP - u2);
        TASK_CHECK(k >= 0);
        u2 += k;
        j += len;
    }
    u1 += lame_encode_flush(g1, a + u1, TEST_CAP - u1);
    u2 += lame_encode_flush(g2, b + u2, TEST_CAP - u2);
    lame_close(g1);
    lame_close(g2);

    /* independent reference runs */
    TASK_CHECK(encode_planar(&cfg, &s1, 1, 1, l, r, TEST_SAMPLES, ra) == u1);
    TASK_CHECK(encode_planar(&cfg, &s2, 1, 1, l, r, TEST_SAMPLES, rb) == u2);
    TASK_CHECK(memcmp(a, ra, (size_t) u1) == 0);
    TASK_CHECK(memcmp(b, rb, (size_t) u2) == 0);
    free(a);
    free(b);
    free(ra);
    free(rb);
    free(l);
    free(r);
    return 0;
}

static int
case_streaming_api_contract(void)
{
    static unsigned char x[32];
    lame_t g = lame_init();
    lame_ancillary_slot bad[2];
    lame_ancillary_slot good;
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };

    fill_slot_data(x, 32, 1);

    /* half null input */
    TASK_CHECK(lame_set_ancillary_slots(g, NULL, 2) == -1);

    /* size out of range */
    bad[0].audio_frame = 0;
    bad[0].data = x;
    bad[0].size = 0;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 1) == -1);
    bad[0].size = 33;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 1) == -1);

    /* null data */
    bad[0].size = 5;
    bad[0].data = NULL;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 1) == -1);

    /* frame numbers must be strictly increasing */
    bad[0].data = x;
    bad[0].audio_frame = 5;
    bad[1].audio_frame = 5;
    bad[1].data = x;
    bad[1].size = 2;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);
    bad[1].audio_frame = 4;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);

    /* a good plan installs, and a later failure must leave it in place */
    good.audio_frame = 0;
    good.data = x;
    good.size = 5;
    TASK_CHECK(lame_set_ancillary_slots(g, &good, 1) == 0);
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);

    /* NULL/0 clears */
    TASK_CHECK(lame_set_ancillary_slots(g, NULL, 0) == 0);

    /* setting after init_params is rejected */
    cfg_encoder(g, &cfg);
    TASK_CHECK(lame_init_params(g) == 0);
    TASK_CHECK(lame_set_ancillary_slots(g, &good, 1) == -1);
    lame_close(g);
    return 0;
}

static int
case_streaming_invalid_then_continue(void)
{
    static unsigned char keep[9], other[4];
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot good, bad[2];
    lame_t g;
    int used = 0, j, k, nall, naudio;

    fill_slot_data(keep, 9, 21);
    fill_slot_data(other, 4, 77);
    gen_pattern(l, r, TEST_SAMPLES);

    g = lame_init();
    cfg_encoder(g, &cfg);
    good.audio_frame = 2;
    good.data = keep;
    good.size = 9;
    TASK_CHECK(lame_set_ancillary_slots(g, &good, 1) == 0);

    /* every rejected call must leave the original plan in place */
    bad[0].audio_frame = 2;
    bad[0].data = other;
    bad[0].size = 4;
    bad[1].audio_frame = 2;
    bad[1].data = other;
    bad[1].size = 4;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);    /* duplicate frame */
    bad[1].audio_frame = 1;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);    /* not increasing */
    bad[0].size = 0;
    TASK_CHECK(lame_set_ancillary_slots(g, bad, 2) == -1);    /* bad size */
    TASK_CHECK(lame_set_ancillary_slots(g, NULL, 3) == -1);   /* half null */

    TASK_CHECK(lame_init_params(g) == 0);
    for (j = 0; j < TEST_SAMPLES;) {
        int len = 1 + (j % 2049);
        if (len > TEST_SAMPLES - j)
            len = TEST_SAMPLES - j;
        k = lame_encode_buffer(g, l + j, r + j, len, out + used, TEST_CAP - used);
        TASK_CHECK(k >= 0);
        used += k;
        j += len;
    }
    k = lame_encode_flush(g, out + used, TEST_CAP - used);
    TASK_CHECK(k >= 0);
    used += k;
    lame_close(g);

    /* the retained original slot must still be delivered, not the bad one */
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio > 3);
    TASK_CHECK(memcmp(out + audio[2].off + audio[2].len - 9, keep, 9) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

static int
case_streaming_empty_plan_and_caller_memory(void)
{
    struct enc_cfg cfg = { 44100, 2, STEREO, 128, 0, 0 };
    unsigned char *a = malloc(TEST_CAP), *b = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    unsigned char *local = malloc(8);
    lame_ancillary_slot slot;
    lame_t g;
    int ua, ub, used = 0, j, k;

    gen_pattern(l, r, TEST_SAMPLES);

    /* no plan at all */
    ua = encode_planar(&cfg, NULL, 0, 1, l, r, TEST_SAMPLES, a);

    /* explicit empty plan must give the byte identical legacy stream */
    g = lame_init();
    cfg_encoder(g, &cfg);
    TASK_CHECK(lame_set_ancillary_slots(g, NULL, 0) == 0);
    TASK_CHECK(lame_init_params(g) == 0);
    for (j = 0; j < TEST_SAMPLES;) {
        int len = 1 + (j % 2049);
        if (len > TEST_SAMPLES - j)
            len = TEST_SAMPLES - j;
        k = lame_encode_buffer(g, l + j, r + j, len, b + used, TEST_CAP - used);
        TASK_CHECK(k >= 0);
        used += k;
        j += len;
    }
    used += lame_encode_flush(g, b + used, TEST_CAP - used);
    lame_close(g);
    TASK_CHECK(used == ua);
    TASK_CHECK(memcmp(a, b, (size_t) ua) == 0);

    /* the library owns an independent copy: mutate and free caller memory */
    memcpy(local, "ABCDEFGH", 8);
    slot.audio_frame = 1;
    slot.data = local;
    slot.size = 8;
    g = lame_init();
    cfg_encoder(g, &cfg);
    TASK_CHECK(lame_set_ancillary_slots(g, &slot, 1) == 0);
    memset(local, 0, 8);
    free(local);
    TASK_CHECK(lame_init_params(g) == 0);
    used = 0;
    for (j = 0; j < TEST_SAMPLES;) {
        int len = 1 + (j % 2049);
        if (len > TEST_SAMPLES - j)
            len = TEST_SAMPLES - j;
        k = lame_encode_buffer(g, l + j, r + j, len, b + used, TEST_CAP - used);
        TASK_CHECK(k >= 0);
        used += k;
        j += len;
    }
    used += lame_encode_flush(g, b + used, TEST_CAP - used);
    lame_close(g);
    {
        struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
        int nall = parse_stream(b, used, all, MAX_FRAMES);
        int naudio = audio_frames(all, nall, audio, MAX_FRAMES);
        TASK_CHECK(naudio > 2);
        TASK_CHECK(memcmp(b + audio[1].off + audio[1].len - 8, "ABCDEFGH", 8) == 0);
    }
    free(a);
    free(b);
    free(l);
    free(r);
    return 0;
}

static int
case_streaming_consecutive_and_ends(void)
{
    static unsigned char d[32];
    struct enc_cfg cfg = { 24000, 1, MONO, 64, 0, 0 };
    struct parsed_frame all[MAX_FRAMES], audio[MAX_FRAMES];
    unsigned char *out = malloc(TEST_CAP);
    short *l = calloc(TEST_SAMPLES, sizeof(short));
    short *r = calloc(TEST_SAMPLES, sizeof(short));
    lame_ancillary_slot slots[6];
    struct slot_expect expect[5];
    int nall, naudio, used, i, last;

    fill_slot_data(d, 32, 8);
    gen_noise(l, r, TEST_SAMPLES, 888);

    /* first frame, three consecutive frames, the last flush audio frame
     * (filled in after we know the frame count) and a beyond-end entry */
    slots[0].audio_frame = 0; slots[0].data = d; slots[0].size = 8;
    slots[1].audio_frame = 1; slots[1].data = d; slots[1].size = 8;
    slots[2].audio_frame = 2; slots[2].data = d; slots[2].size = 8;
    slots[3].audio_frame = 4096; slots[3].data = d; slots[3].size = 4;  /* beyond end */
    slots[4].audio_frame = 4097; slots[4].data = d; slots[4].size = 4;  /* beyond end */

    used = encode_planar(&cfg, slots, 5, 1, l, r, TEST_SAMPLES, out);
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio > 5);
    last = naudio - 1;

    /* add a slot on the last flush frame, re-encode, and prove no extra
     * audio frame is produced for the beyond-end entries */
    slots[5].audio_frame = (unsigned long) last;
    slots[5].data = d;
    slots[5].size = 32;
    /* keep strictly increasing: rebuild the plan in frame order */
    {
        lame_ancillary_slot ordered[6];
        ordered[0] = slots[0];
        ordered[1] = slots[1];
        ordered[2] = slots[2];
        ordered[3] = slots[5];
        ordered[4] = slots[3];
        ordered[5] = slots[4];
        used = encode_planar(&cfg, ordered, 6, 1, l, r, TEST_SAMPLES, out);
    }
    nall = parse_stream(out, used, all, MAX_FRAMES);
    naudio = audio_frames(all, nall, audio, MAX_FRAMES);
    TASK_CHECK(naudio == last + 1);   /* no extra frames appended */

    for (i = 0; i < 3; ++i) {
        expect[i].frame = (unsigned long) i;
        expect[i].size = 8;
        expect[i].data = d;
    }
    expect[3].frame = (unsigned long) last;
    expect[3].size = 32;
    expect[3].data = d;
    TASK_CHECK(verify_geometry(out, audio, naudio, expect, 4) == 0);
    TASK_CHECK(memcmp(out + audio[last].off + audio[last].len - 32, d, 32) == 0);
    free(out);
    free(l);
    free(r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    /* placement */
    { "placement", "tail_bytes",            case_placement_tail_bytes},
    { "placement", "crc_padding",           case_placement_crc_padding},
    { "placement", "xing_id3_numbering",    case_placement_xing_id3},
    { "placement", "decode_invariant",      case_placement_decode_invariant},
    { "placement", "config_support",        case_placement_config_support},
    /* reservoir */
    { "reservoir", "real_borrowing",        case_reservoir_real_borrowing},
    { "reservoir", "drain_and_rebuild",     case_reservoir_drain_and_rebuild},
    { "reservoir", "slot_in_noisy_region",  case_reservoir_slot_in_noisy_region},
    /* streaming */
    { "streaming", "chunk_equivalence",     case_streaming_chunk_equivalence},
    { "streaming", "dual_instance",         case_streaming_dual_instance},
    { "streaming", "api_contract",          case_streaming_api_contract},
    { "streaming", "invalid_then_continue", case_streaming_invalid_then_continue},
    { "streaming", "empty_plan_and_memory", case_streaming_empty_plan_and_caller_memory},
    { "streaming", "consecutive_and_ends",  case_streaming_consecutive_and_ends},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
