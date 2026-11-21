/*
 * CRC error-protection recovery tests for MINIMP3_CHECK_CRC.
 * Registered through task_support/check.h in three groups:
 *   protected - CRC status semantics, independent bitwise CRC, protection scope
 *   recovery  - bad-frame muting, compression/audio history handling
 *   extended  - mp3dec_ex streaming, small reads, callback IO and seeking
 */
#include "../task_support/check.h"
#include "minimp3.h"
#include "minimp3_ex.h"
#include "../devcheck/reference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VECTOR_PATH "vectors/l3-hecommon.bit"
#define M2_PATH      "vectors/M2L3_noise.bit"

/* ------------------------------------------------------------------ */
/* independent bit-level CRC: init 0xffff, poly 0x8005, MSB first,   */
/* no final xor. Written independently from the production code.     */
/* ------------------------------------------------------------------ */
static uint16_t test_crc16(const uint8_t *data, int nbits)
{
    uint16_t crc = 0xffff;
    int b;
    for (b = 0; b < nbits; b++)
    {
        unsigned inbit = (unsigned)((data[b >> 3] >> (7 - (b & 7))) & 1);
        unsigned top = (unsigned)(crc >> 15);
        crc = (uint16_t)((crc << 1) ^ ((inbit ^ top) ? 0x8005u : 0u));
    }
    return crc;
}

static int test_si_bytes(const uint8_t *h)
{
    int mpeg1 = (h[1] & 0x08) != 0;
    int mono = ((h[3] & 0xC0) == 0xC0);
    return mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
}

/* local MSB-first bit reader, independent of the decoder internals */
struct rb { const uint8_t *p; int pos; };

static unsigned rb_get(struct rb *r, int n)
{
    unsigned v = 0;
    while (n--)
    {
        v = (v << 1) | ((r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
        r->pos++;
    }
    return v;
}

static int frame_main_data_begin(const uint8_t *hdr, int protected_)
{
    struct rb r;
    r.p = hdr + 4 + (protected_ ? 2 : 0);
    r.pos = 0;
    if (hdr[1] & 0x08) /* MPEG-1 */
        return (int)rb_get(&r, 9);
    int gr = ((hdr[3] & 0xC0) == 0xC0) ? 1 : 2;
    return (int)(rb_get(&r, 8 + gr) >> gr);
}

static uint16_t test_frame_crc(const uint8_t *frame, int frame_size)
{
    int sib = test_si_bytes(frame);
    uint8_t blob[2 + 32];
    blob[0] = frame[2];
    blob[1] = frame[3];
    memcpy(blob + 2, frame + 6, sib);
    (void)frame_size;
    return test_crc16(blob, (2 + sib) * 8);
}

static int load_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return -1; }
    uint8_t *b = (uint8_t*)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    *out = b;
    *out_size = (size_t)sz;
    return 0;
}

struct frame_desc
{
    size_t off;
    int frame_bytes, channels, si_bytes, main_data_begin, protected_, layer, samples;
};

static int build_frame_table(const uint8_t *buf, size_t size, struct frame_desc *out, int maxn)
{
    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t off = 0;
    int n = 0;
    while (off < size && n < maxn)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&dec, buf + off, (int)(size - off), NULL, &info);
        if (!info.frame_bytes) break;
        const uint8_t *hdr = buf + off + info.frame_offset;
        struct frame_desc *d = &out[n];
        d->off = off;
        d->frame_bytes = info.frame_bytes;
        d->channels = info.channels;
        d->si_bytes = test_si_bytes(hdr);
        d->protected_ = (info.layer == 3 && !(hdr[1] & 1));
        d->layer = info.layer;
        d->samples = samples;
        d->main_data_begin = -1;
        if (info.layer == 3)
            d->main_data_begin = frame_main_data_begin(hdr, d->protected_);
        off += info.frame_bytes;
        n++;
    }
    return n;
}

static int all_zero(const mp3d_sample_t *pcm, int n)
{
    int i;
    for (i = 0; i < n; i++) if (pcm[i] != 0) return 0;
    return 1;
}

static int any_nonzero(const mp3d_sample_t *pcm, int n)
{
    return !all_zero(pcm, n);
}

/* ============================ protected ============================ */

static int run_status_basics(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    TASK_CHECK(nframes == 30);

    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t off = 0;
    int k, protected_count = 0, plain_count = 0, total_scalars = 0;
    for (k = 0; k < nframes; k++)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0x5a, sizeof(info)); /* poison: crc_status must be (re)initialized */
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&dec, buf + off, (int)(size - off), pcm, &info);
        TASK_CHECK(info.frame_bytes == frames[k].frame_bytes);
        TASK_CHECK(samples == frames[k].samples && samples == 1152);
        if (frames[k].protected_)
        {
            TASK_CHECK(info.crc_status == 1);
            protected_count++;
        } else
        {
            TASK_CHECK(info.crc_status == 0);
            plain_count++;
        }
        total_scalars += samples * info.channels;
        off += info.frame_bytes;
    }
    TASK_CHECK(protected_count == 25 && plain_count == 5);
    TASK_CHECK(total_scalars == 69120);
    free(buf);
    return 0;
}

static int run_crc_bitwise(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    uint8_t *work = (uint8_t*)malloc(size);
    TASK_CHECK(work != NULL);
    int k;
    for (k = 0; k < nframes; k++)
    {
        const struct frame_desc *d = &frames[k];
        const uint8_t *frame = buf + d->off;
        if (!d->protected_) continue;
        uint16_t stored = (uint16_t)((frame[4] << 8) | frame[5]);
        /* independent recomputation must match the stored word */
        TASK_CHECK(test_frame_crc(frame, d->frame_bytes) == stored);

        /* flipping either stored-CRC byte must make the stored word disagree
           with the recomputed CRC (the CRC word itself is skipped in the sum) */
        memcpy(work, buf, size);
        work[d->off + 4] ^= 0x80;
        uint16_t stored4 = (uint16_t)((work[d->off + 4] << 8) | work[d->off + 5]);
        TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) != stored4);
        memcpy(work, buf, size);
        work[d->off + 5] ^= 0x01;
        uint16_t stored5 = (uint16_t)((work[d->off + 4] << 8) | work[d->off + 5]);
        TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) != stored5);

        /* flipping any side-information byte must be detected */
        int s;
        for (s = 0; s < d->si_bytes; s++)
        {
            memcpy(work, buf, size);
            work[d->off + 6 + s] ^= 0x80;
            TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) != stored);
        }
        /* protected header bits: private bit of h[2] and bits of h[3] */
        memcpy(work, buf, size);
        work[d->off + 2] ^= 0x01;
        TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) != stored);
        memcpy(work, buf, size);
        work[d->off + 3] ^= 0x04;
        TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) != stored);

        /* production decoder must agree with the independent recomputation */
        {
            mp3dec_t dec;
            mp3dec_frame_info_t info;
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            mp3dec_init(&dec);
            memset(&info, 0, sizeof(info));
            mp3dec_decode_frame(&dec, buf + d->off, d->frame_bytes, pcm, &info);
            TASK_CHECK(info.crc_status == 1);

            memcpy(work, buf, size);
            work[d->off + 4] ^= 0x01;                 /* stored CRC word */
            mp3dec_init(&dec);
            memset(&info, 0, sizeof(info));
            mp3dec_decode_frame(&dec, work + d->off, d->frame_bytes, pcm, &info);
            TASK_CHECK(info.crc_status == -1);

            memcpy(work, buf, size);
            work[d->off + 6] ^= 0x01;                 /* side information */
            mp3dec_init(&dec);
            memset(&info, 0, sizeof(info));
            mp3dec_decode_frame(&dec, work + d->off, d->frame_bytes, pcm, &info);
            TASK_CHECK(info.crc_status == -1);
        }
    }
    free(work);
    free(buf);
    return 0;
}

static int run_scope_maindata(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    mp3dec_t dec;
    int k;
    /* a main-data change must leave the CRC valid: protection does not cover main data */
    for (k = 0; k < nframes; k++)
    {
        const struct frame_desc *d = &frames[k];
        if (!d->protected_) continue;
        int main_off = d->off + 6 + d->si_bytes;
        TASK_CHECK((size_t)main_off + 4 < d->off + (size_t)d->frame_bytes);
        uint8_t *work = (uint8_t*)malloc(size);
        memcpy(work, buf, size);
        work[main_off] ^= 0xA5;
        work[main_off + 1] ^= 0x5A;
        work[main_off + 3] ^= 0xFF;
        uint16_t stored = (uint16_t)((work[d->off + 4] << 8) | work[d->off + 5]);
        TASK_CHECK(test_frame_crc(work + d->off, d->frame_bytes) == stored);

        mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_decode_frame(&dec, work + d->off, d->frame_bytes, pcm, &info);
        /* main data sits outside the protected scope: frame still recognized, CRC passes */
        TASK_CHECK(info.frame_bytes == d->frame_bytes && info.crc_status == 1);
        free(work);
    }
    free(buf);
    return 0;
}

/* build a legal small CRC-protected frame: header + CRC + zero side info + zero main data */
static int build_synth_frame(uint8_t *out, int h1, int bitrate_idx, int sr_idx, int mode_byte)
{
    /* MPEG-2/2.5 Layer III bitrate table in kbps (ISO halfrate table, already doubled) */
    static const unsigned br_l3_m2[15] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160 };
    int mpeg25 = (h1 == 0xe2);
    unsigned hz = mpeg25 ? 11025u : 22050u;
    unsigned kbps = br_l3_m2[bitrate_idx];
    int frame_bytes = (int)(576u * kbps * 125u / hz);
    memset(out, 0, frame_bytes);
    out[0] = 0xff;
    out[1] = (uint8_t)h1;                 /* protection bit zero -> CRC present */
    out[2] = (uint8_t)((bitrate_idx << 4) | (sr_idx << 2));
    out[3] = (uint8_t)mode_byte;
    uint16_t crc = test_frame_crc(out, frame_bytes);
    out[4] = (uint8_t)(crc >> 8);
    out[5] = (uint8_t)crc;
    return frame_bytes;
}

static int run_synth_structure(void)
{
    /* MPEG-2 mono (si=9), MPEG-2 stereo (si=17), MPEG-2.5 mono (si=9) */
    uint8_t frame[1024];
    int t;
    build_synth_frame(frame, 0xf2, 8, 0, 0xC0);
    TASK_CHECK(test_si_bytes(frame) == 9);

    struct { int h1; int mode; int expect_si; } cases[3] = {
        { 0xf2, 0xC0, 9 }, { 0xf2, 0x40, 17 }, { 0xe2, 0xC0, 9 }
    };
    for (t = 0; t < 3; t++)
    {
        int fsize = build_synth_frame(frame, cases[t].h1, 8, 0, cases[t].mode);
        TASK_CHECK(test_si_bytes(frame) == cases[t].expect_si);
        mp3dec_t dec;
        mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&dec, frame, fsize, pcm, &info);
        TASK_CHECK(samples == 576 && info.frame_bytes == fsize);
        TASK_CHECK(info.crc_status == 1);
        TASK_CHECK(all_zero(pcm, samples * info.channels));

        /* every side-info byte is inside the protected boundary */
        int s;
        for (s = 0; s < cases[t].expect_si; s++)
        {
            uint8_t work[1024];
            build_synth_frame(work, cases[t].h1, 8, 0, cases[t].mode);
            work[6 + s] ^= 0x80;
            mp3dec_init(&dec);
            memset(&info, 0, sizeof(info));
            mp3dec_decode_frame(&dec, work, fsize, pcm, &info);
            TASK_CHECK(info.crc_status == -1);
        }
        /* the first byte after side information is main data: outside the boundary */
        {
            uint8_t work[1024];
            build_synth_frame(work, cases[t].h1, 8, 0, cases[t].mode);
            work[6 + cases[t].expect_si] ^= 0x80;
            mp3dec_init(&dec);
            memset(&info, 0, sizeof(info));
            mp3dec_decode_frame(&dec, work, fsize, pcm, &info);
            TASK_CHECK(info.crc_status == 1);
        }
    }

    /* MPEG-1 mono: 17-byte side information boundary, 1152 samples */
    {
        uint8_t frame[1024];
        int fsize = 144 * 128000 / 44100;
        memset(frame, 0, sizeof(frame));
        frame[0] = 0xff; frame[1] = 0xfa; /* MPEG1 L3, CRC present */
        frame[2] = 0x90; frame[3] = 0xC0; /* 128 kbps, 44.1kHz, mono */
        TASK_CHECK(test_si_bytes(frame) == 17);
        uint16_t c = test_frame_crc(frame, fsize);
        frame[4] = (uint8_t)(c >> 8); frame[5] = (uint8_t)c;
        mp3dec_t dec;
        mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        TASK_CHECK(mp3dec_decode_frame(&dec, frame, fsize, pcm, &info) == 1152);
        TASK_CHECK(info.channels == 1 && info.crc_status == 1);
        frame[6 + 16] ^= 0x80; /* last side-info byte */
        mp3dec_init(&dec);
        memset(&info, 0, sizeof(info));
        mp3dec_decode_frame(&dec, frame, fsize, pcm, &info);
        TASK_CHECK(info.crc_status == -1);
    }
    return 0;
}

static int run_other_layer_unchecked(void)
{
    /* CRC-protected Layer II frame must keep crc_status == 0: only Layer III is covered */
    uint8_t frame[1024];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xff;
    frame[1] = 0xfc; /* MPEG1 Layer II, protection bit zero */
    frame[2] = 0xA0; /* 192 kbps (L2 index 10), 44.1kHz, no padding */
    frame[3] = 0xC0; /* mono */
    int frame_bytes = 144 * 192000 / 44100;
    TASK_CHECK(frame_bytes < (int)sizeof(frame));
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    memset(&info, 0x5a, sizeof(info));
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples = mp3dec_decode_frame(&dec, frame, frame_bytes, pcm, &info);
    (void)samples;
    TASK_CHECK(info.frame_bytes == frame_bytes && info.layer == 2);
    TASK_CHECK(info.crc_status == 0);

    /* garbage buffer: still initialized to zero, no full frame -> no silence invented */
    uint8_t junk[64];
    memset(junk, 0x77, sizeof(junk));
    mp3dec_init(&dec);
    memset(&info, 0x5a, sizeof(info));
    TASK_CHECK(mp3dec_decode_frame(&dec, junk, (int)sizeof(junk), pcm, &info) == 0);
    TASK_CHECK(info.crc_status == 0);
    return 0;
}

/* ============================ recovery ============================ */

/* decode the whole stream; returns per-frame status/sample info and full PCM */
struct decode_result
{
    int nframes;
    int status[64];
    int samples[64];
    int frame_bytes[64];
    int total_scalars;
    mp3d_sample_t pcm[64 * MINIMP3_MAX_SAMPLES_PER_FRAME];
};

static void decode_stream(const uint8_t *buf, size_t size, struct decode_result *r)
{
    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t off = 0;
    r->nframes = 0;
    r->total_scalars = 0;
    while (off < size && r->nframes < 64)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int idx = r->nframes;
        int samples = mp3dec_decode_frame(&dec, buf + off, (int)(size - off),
                                         r->pcm + r->total_scalars, &info);
        if (!info.frame_bytes) break;
        r->status[idx] = info.crc_status;
        r->samples[idx] = samples;
        r->frame_bytes[idx] = info.frame_bytes;
        r->total_scalars += samples * info.channels;
        off += info.frame_bytes;
        r->nframes++;
    }
}

static int run_good_bad_good(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    TASK_CHECK(nframes == 30);
    const int bad = 5;
    TASK_CHECK(frames[bad].protected_);
    TASK_CHECK(frames[bad + 1].main_data_begin > 0); /* following frame uses the reservoir */

    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01; /* side-info bit: CRC fails, sync intact */

    struct decode_result good_res, bad_res;
    decode_stream(buf, size, &good_res);
    decode_stream(corrupt, size, &bad_res);

    /* timeline is identical: same frame count, same physical consumption, same sample counts */
    TASK_CHECK(bad_res.nframes == good_res.nframes);
    TASK_CHECK(bad_res.total_scalars == good_res.total_scalars);
    int k;
    for (k = 0; k < nframes; k++)
    {
        TASK_CHECK(bad_res.frame_bytes[k] == good_res.frame_bytes[k]);
        TASK_CHECK(bad_res.samples[k] == good_res.samples[k]);
    }
    /* bad frame: reported mismatch and exactly one frame of pure zero PCM */
    TASK_CHECK(bad_res.status[bad] == -1);
    int scalars_per_frame = bad_res.samples[bad] * frames[bad].channels;
    TASK_CHECK(all_zero(bad_res.pcm + bad * scalars_per_frame, scalars_per_frame));
    /* frame 4 carries no protection bit (status 0); the next protected frame passes */
    TASK_CHECK(bad_res.status[bad - 1] == 0 && bad_res.status[bad + 1] == 1);
    TASK_CHECK(any_nonzero(bad_res.pcm + (bad + 1) * scalars_per_frame, scalars_per_frame));
    /* prefix before the fault is bit-identical to the uncorrupted decode */
    TASK_CHECK(memcmp(bad_res.pcm, good_res.pcm, bad * scalars_per_frame) == 0);

    /* independent reference (CRC-less build): same timeline, manual audio-history reset */
    reference_mp3dec_t rdec;
    reference_mp3dec_init(&rdec);
    size_t off = 0;
    for (k = 0; k < nframes; k++)
    {
        reference_mp3dec_frame_info_t rinfo;
        memset(&rinfo, 0, sizeof(rinfo));
        short rpcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int rn = reference_mp3dec_decode_frame(&rdec, corrupt + off, (int)(size - off), rpcm, &rinfo);
        TASK_CHECK(rinfo.frame_bytes == bad_res.frame_bytes[k]);
        TASK_CHECK(rn == bad_res.samples[k] && rinfo.channels == frames[k].channels);
        if (k == bad)
        {
            /* reference emulation: mute frame and restart audio history at the fault */
            TASK_CHECK(all_zero(bad_res.pcm + k * scalars_per_frame, scalars_per_frame));
            memset(rdec.mdct_overlap, 0, sizeof(rdec.mdct_overlap));
            memset(rdec.qmf_state, 0, sizeof(rdec.qmf_state));
        }
        off += rinfo.frame_bytes;
    }

    /* audio history restarts at the fault boundary: after the bad frame the
       decoder's overlap/QMF state is all-zero, so the next granule is built from
       a cleared history rather than pre-fault sound */
    mp3dec_t cont;
    mp3dec_init(&cont);
    short pc[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t inf;
    off = 0;
    for (k = 0; k <= bad; k++)
    {
        memset(&inf, 0, sizeof(inf));
        mp3dec_decode_frame(&cont, corrupt + off, (int)(size - off), pc, &inf);
        off += inf.frame_bytes;
    }
    {
        float zeros_mdct[2][9 * 32], zeros_qmf[15 * 2 * 32];
        memset(zeros_mdct, 0, sizeof(zeros_mdct));
        memset(zeros_qmf, 0, sizeof(zeros_qmf));
        TASK_CHECK(memcmp(cont.mdct_overlap, zeros_mdct, sizeof(zeros_mdct)) == 0);
        TASK_CHECK(memcmp(cont.qmf_state, zeros_qmf, sizeof(zeros_qmf)) == 0);
    }
    /* next good frame decodes with rebuilt history and nonzero output */
    memset(&inf, 0, sizeof(inf));
    int cn = mp3dec_decode_frame(&cont, corrupt + off, (int)(size - off), pc, &inf);
    TASK_CHECK(cn == 1152 && inf.crc_status == 1 && any_nonzero(pc, cn * inf.channels));

    free(corrupt);
    free(buf);
    return 0;
}

static int run_reservoir_payload(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 7] ^= 0x01;

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    size_t off = 0;
    int k;
    for (k = 0; k <= bad; k++)
    {
        memset(&info, 0, sizeof(info));
        mp3dec_decode_frame(&dec, corrupt + off, (int)(size - off),
                            (short[MINIMP3_MAX_SAMPLES_PER_FRAME]){0}, &info);
        off += info.frame_bytes;
    }
    /* bounded history: reservoir never exceeds 511 bytes */
    TASK_CHECK(dec.reserv > 0 && dec.reserv <= 511);
    /* the bad frame's physical main-data tail must be present in the compression history */
    const struct frame_desc *d = &frames[bad];
    int main_off = d->off + 6 + d->si_bytes;
    int main_bytes = d->frame_bytes - 6 - d->si_bytes;
    int tail = main_bytes < dec.reserv ? main_bytes : dec.reserv;
    TASK_CHECK(memcmp(dec.reserv_buf + dec.reserv - tail,
                      corrupt + main_off + main_bytes - tail, tail) == 0);
    /* audio history was restarted at the fault */
    float zeros_mdct[2][9 * 32], zeros_qmf[15 * 2 * 32];
    memset(zeros_mdct, 0, sizeof(zeros_mdct));
    memset(zeros_qmf, 0, sizeof(zeros_qmf));
    TASK_CHECK(memcmp(dec.mdct_overlap, zeros_mdct, sizeof(zeros_mdct)) == 0);
    TASK_CHECK(memcmp(dec.qmf_state, zeros_qmf, sizeof(zeros_qmf)) == 0);
    (void)nframes;
    free(corrupt);
    free(buf);
    return 0;
}

static int run_consecutive_bad(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    int b;
    for (b = 5; b <= 7; b++)
        corrupt[frames[b].off + 6 + b] ^= 0x01; /* distinct SI bytes, frames 5..7 */

    struct decode_result r;
    decode_stream(corrupt, size, &r);
    TASK_CHECK(r.nframes == nframes && r.total_scalars == 69120);
    int scalars = r.samples[0] * frames[0].channels;
    for (b = 5; b <= 7; b++)
    {
        TASK_CHECK(r.status[b] == -1);
        TASK_CHECK(all_zero(r.pcm + b * scalars, scalars));
    }
    TASK_CHECK(r.status[4] == 0 && r.status[8] == 1);
    TASK_CHECK(any_nonzero(r.pcm + 8 * scalars, scalars));

    /* audio history remains cleared across consecutive faults */
    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t off = 0;
    int k;
    for (k = 0; k <= 7; k++)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_decode_frame(&dec, corrupt + off, (int)(size - off), pcm, &info);
        off += info.frame_bytes;
    }
    float z[15 * 2 * 32];
    memset(z, 0, sizeof(z));
    TASK_CHECK(memcmp(dec.qmf_state, z, sizeof(z)) == 0);
    TASK_CHECK(dec.reserv > 0 && dec.reserv <= 511);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_pseudo_sync(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int target = 10;
    TASK_CHECK(frames[target].protected_);
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    /* plant a valid-looking sync word inside main data; CRC scope excludes main data */
    int p = frames[target].off + 6 + frames[target].si_bytes + 8;
    corrupt[p + 0] = 0xff;
    corrupt[p + 1] = 0xfb;
    corrupt[p + 2] = 0x90;
    corrupt[p + 3] = 0x00;

    struct decode_result r;
    decode_stream(corrupt, size, &r);
    TASK_CHECK(r.nframes == nframes);
    TASK_CHECK(r.status[target] == 1);                /* main data is not protected... */
    TASK_CHECK(r.frame_bytes[target] == frames[target].frame_bytes); /* ...and no resync */
    int k;
    for (k = 0; k < nframes; k++)
        TASK_CHECK(r.frame_bytes[k] == frames[k].frame_bytes);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_truncated(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    /* half a frame must follow the incomplete-frame path: no fabricated silence */
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    memset(&info, 0x5a, sizeof(info));
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int half = 200;
    TASK_CHECK(half < frames[bad].frame_bytes);
    int samples = mp3dec_decode_frame(&dec, corrupt + frames[bad].off, half, pcm, &info);
    TASK_CHECK(samples == 0);
    TASK_CHECK(info.crc_status == 0);

    /* feeding the rest of the stream afterwards still synchronizes on real frames */
    memset(&info, 0, sizeof(info));
    samples = mp3dec_decode_frame(&dec, corrupt + frames[bad].off + half,
                                  (int)(size - frames[bad].off - half), pcm, &info);
    TASK_CHECK(samples == 0 || info.frame_bytes > 0); /* no crash, ordinary skip/decode */
    free(corrupt);
    free(buf);
    return 0;
}

static int run_info_only(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    /* pcm == NULL still reports the verdict, but advances no history */
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    size_t off = 0;
    int k;
    for (k = 0; k <= bad; k++)
    {
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&dec, corrupt + off, (int)(size - off), NULL, &info);
        TASK_CHECK(samples == frames[k].samples);
        TASK_CHECK(info.crc_status == (k == bad ? -1 : (frames[k].protected_ ? 1 : 0)));
        off += info.frame_bytes;
    }
    /* the info-only bad-frame call must not have touched compression/audio history */
    TASK_CHECK(dec.reserv == 0);
    {
        float z_mdct[2][9 * 32], z_qmf[15 * 2 * 32];
        memset(z_mdct, 0, sizeof(z_mdct));
        memset(z_qmf, 0, sizeof(z_qmf));
        TASK_CHECK(memcmp(dec.mdct_overlap, z_mdct, sizeof(z_mdct)) == 0);
        TASK_CHECK(memcmp(dec.qmf_state, z_qmf, sizeof(z_qmf)) == 0);
    }
    /* same decoder now decodes sound: info-only calls left no compression history behind,
       so the bad frame is handled afresh (mute, then bounded history append) */
    mp3dec_init(&dec);
    off = 0;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (k = 0; k <= bad + 1; k++)
    {
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&dec, corrupt + off, (int)(size - off), pcm, &info);
        if (k == bad)
        {
            TASK_CHECK(info.crc_status == -1 && all_zero(pcm, samples * info.channels));
        } else if (k == bad + 1)
        {
            TASK_CHECK(info.crc_status == 1 && any_nonzero(pcm, samples * info.channels));
        }
        off += info.frame_bytes;
    }
    free(corrupt);
    free(buf);
    return 0;
}

/* ============================ extended ============================ */

static int run_ex_buf_timeline(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    mp3dec_ex_t dec;
    TASK_CHECK(mp3dec_ex_open_buf(&dec, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    int scalars = 1152 * 2;
    mp3d_sample_t *out = (mp3d_sample_t*)calloc(nframes * scalars, sizeof(mp3d_sample_t));
    size_t got = mp3dec_ex_read(&dec, out, nframes * scalars);
    TASK_CHECK(got == (size_t)nframes * scalars); /* silence counts, not EOF */
    TASK_CHECK(dec.last_error == 0);
    TASK_CHECK(all_zero(out + bad * scalars, scalars));
    TASK_CHECK(any_nonzero(out + (bad + 1) * scalars, scalars));
    TASK_CHECK(any_nonzero(out, scalars));
    mp3dec_ex_close(&dec);
    free(out);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_ex_small_reads(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    mp3dec_ex_t dec;
    TASK_CHECK(mp3dec_ex_open_buf(&dec, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    int expected[64], k;
    for (k = 0; k < nframes; k++)
        expected[k] = frames[k].protected_ ? (k == bad ? -1 : 1) : 0;

    int per_frame = 2304, frame_idx = -1, seen = 0;
    int frame_status[64], partial_counts[64];
    memset(frame_status, 0, sizeof(frame_status));
    memset(partial_counts, 0, sizeof(partial_counts));
    for (;;)
    {
        mp3d_sample_t *fbuf = NULL;
        mp3dec_frame_info_t info;
        info.crc_status = 77; /* poison: partial reads must not keep the caller's stale value */
        size_t n = mp3dec_ex_read_frame(&dec, &fbuf, &info, 100);
        if (!n) break;
        int which = (int)(dec.cur_sample - n) / per_frame;
        TASK_CHECK(which >= 0 && which < nframes);
        if (which != frame_idx)
        {
            frame_idx = which;
            frame_status[which] = info.crc_status;
        } else
        {
            TASK_CHECK(info.crc_status == frame_status[which]); /* stays owned by this frame */
        }
        partial_counts[which]++;
        seen += (int)n;
    }
    TASK_CHECK(seen == nframes * per_frame);
    for (k = 0; k < nframes; k++)
    {
        TASK_CHECK(frame_status[k] == expected[k]);
        TASK_CHECK(partial_counts[k] >= 2); /* every frame was split across small reads */
    }
    mp3dec_ex_close(&dec);
    free(corrupt);
    free(buf);
    return 0;
}

/* ----------------------- callback memory IO ----------------------- */
struct mem_io
{
    const uint8_t *data;
    size_t size, pos;
};

static size_t mem_read(void *dst, size_t size, void *user)
{
    struct mem_io *io = (struct mem_io*)user;
    size_t avail = io->size - io->pos;
    size_t n = size < avail ? size : avail;
    memcpy(dst, io->data + io->pos, n);
    io->pos += n;
    return n;
}

static int mem_seek(uint64_t position, void *user)
{
    struct mem_io *io = (struct mem_io*)user;
    if (position > io->size) return -1;
    io->pos = (size_t)position;
    return 0;
}

static int run_buf_callback_match(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    mp3dec_ex_t dbuf;
    TASK_CHECK(mp3dec_ex_open_buf(&dbuf, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    struct mem_io io = { corrupt, size, 0 };
    mp3dec_io_t io_if = { mem_read, &io, mem_seek, &io };
    mp3dec_ex_t dcb;
    TASK_CHECK(mp3dec_ex_open_cb(&dcb, &io_if, MP3D_SEEK_TO_SAMPLE) == 0);

    int total = nframes * 2304;
    mp3d_sample_t *ab = (mp3d_sample_t*)malloc(total * sizeof(mp3d_sample_t));
    mp3d_sample_t *ac = (mp3d_sample_t*)malloc(total * sizeof(mp3d_sample_t));
    TASK_CHECK(mp3dec_ex_read(&dbuf, ab, total) == (size_t)total);
    TASK_CHECK(mp3dec_ex_read(&dcb, ac, total) == (size_t)total);
    TASK_CHECK(memcmp(ab, ac, total * sizeof(mp3d_sample_t)) == 0);

    /* per-frame status sequence must match too, including small callback reads */
    TASK_CHECK(mp3dec_ex_seek(&dcb, 0) == 0);
    int frame_idx = -1, k = 0;
    int statuses[64];
    memset(statuses, 0, sizeof(statuses));
    for (;;)
    {
        mp3d_sample_t *fbuf = NULL;
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        size_t n = mp3dec_ex_read_frame(&dcb, &fbuf, &info, 137);
        if (!n) break;
        int which = (int)(dcb.cur_sample - n) / 2304;
        if (which != frame_idx) { frame_idx = which; statuses[which] = info.crc_status; }
        else TASK_CHECK(info.crc_status == statuses[which]);
        k++;
    }
    TASK_CHECK(frame_idx == nframes - 1 && k >= nframes);
    for (k = 0; k < nframes; k++)
        TASK_CHECK(statuses[k] == (frames[k].protected_ ? (k == bad ? -1 : 1) : 0));

    mp3dec_ex_close(&dbuf);
    mp3dec_ex_close(&dcb);
    free(ac); free(ab); free(corrupt); free(buf);
    return 0;
}

static int run_seek_bad_and_good(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    struct decode_result cont;
    decode_stream(corrupt, size, &cont);

    mp3dec_ex_t dec;
    TASK_CHECK(mp3dec_ex_open_buf(&dec, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    const int per_frame = 2304;

    /* seek inside the bad frame: the corresponding samples must be zero, status -1 */
    TASK_CHECK(mp3dec_ex_seek(&dec, (uint64_t)bad * per_frame + 128) == 0);
    mp3d_sample_t *fbuf = NULL;
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    size_t n = mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame);
    TASK_CHECK(n > 0 && info.crc_status == -1);
    TASK_CHECK(all_zero(fbuf, n));

    /* seek to the good frame after the fault and compare against the continuous
       timeline; integer sample seek on good frames allows up to 4 LSB difference */
    TASK_CHECK(mp3dec_ex_seek(&dec, (uint64_t)(bad + 1) * per_frame) == 0);
    memset(&info, 0, sizeof(info));
    n = mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame);
    TASK_CHECK(n == (size_t)per_frame && info.crc_status == 1);
    const mp3d_sample_t *ref = cont.pcm + (bad + 1) * per_frame;
    int j, maxdiff = 0;
    for (j = 0; j < per_frame; j++)
    {
        int d = abs((int)fbuf[j] - (int)ref[j]);
        if (d > maxdiff) maxdiff = d;
    }
    TASK_CHECK(maxdiff <= 4);

    /* seek further ahead: pre-roll backtracking must cross the bad frame itself,
       keep the compression data later frames need (main_data_begin == 511) and
       rebuild audio history from the most recent bad frame */
    TASK_CHECK(mp3dec_ex_seek(&dec, (uint64_t)(bad + 3) * per_frame) == 0);
    memset(&info, 0, sizeof(info));
    n = mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame);
    TASK_CHECK(n == (size_t)per_frame && info.crc_status == 1);
    ref = cont.pcm + (bad + 3) * per_frame;
    maxdiff = 0;
    for (j = 0; j < per_frame; j++)
    {
        int d = abs((int)fbuf[j] - (int)ref[j]);
        if (d > maxdiff) maxdiff = d;
    }
    TASK_CHECK(maxdiff <= 4);

    /* the fault frame itself stays exact even through seek pre-roll */
    TASK_CHECK(mp3dec_ex_seek(&dec, (uint64_t)bad * per_frame) == 0);
    memset(&info, 0, sizeof(info));
    n = mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame);
    TASK_CHECK(n == (size_t)per_frame && info.crc_status == -1 && all_zero(fbuf, n));

    /* total length after seeking around is unchanged */
    TASK_CHECK(mp3dec_ex_seek(&dec, 0) == 0);
    mp3d_sample_t *all = (mp3d_sample_t*)malloc(nframes * per_frame * sizeof(mp3d_sample_t));
    size_t got = mp3dec_ex_read(&dec, all, nframes * per_frame);
    TASK_CHECK(got == (size_t)nframes * per_frame && dec.last_error == 0);
    TASK_CHECK(all_zero(all + bad * per_frame, per_frame));
    free(all);
    mp3dec_ex_close(&dec);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_seek_backward(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 6] ^= 0x01;

    mp3dec_ex_t dec;
    TASK_CHECK(mp3dec_ex_open_buf(&dec, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    int per_frame = 2304;

    TASK_CHECK(mp3dec_ex_seek(&dec, 12 * per_frame) == 0);
    mp3d_sample_t *fbuf;
    mp3dec_frame_info_t info;
    TASK_CHECK(mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame) == (size_t)per_frame);
    TASK_CHECK(info.crc_status == 1);

    /* backward seek re-enters the silence region */
    TASK_CHECK(mp3dec_ex_seek(&dec, bad * per_frame) == 0);
    TASK_CHECK(mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame) == (size_t)per_frame);
    TASK_CHECK(info.crc_status == -1 && all_zero(fbuf, per_frame));

    /* seek back to zero: first frame matches the from-start decode exactly */
    struct decode_result cont;
    decode_stream(corrupt, size, &cont);
    TASK_CHECK(mp3dec_ex_seek(&dec, 0) == 0);
    TASK_CHECK(mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame) == (size_t)per_frame);
    TASK_CHECK(memcmp(fbuf, cont.pcm, per_frame * sizeof(mp3d_sample_t)) == 0);
    TASK_CHECK(info.crc_status == 0); /* first frames carry no protection bit */
    (void)nframes;
    mp3dec_ex_close(&dec);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_seek_consecutive_bad(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    int b;
    for (b = 5; b <= 6; b++)
        corrupt[frames[b].off + 6 + b] ^= 0x01; /* two consecutive bad frames */

    struct decode_result cont;
    decode_stream(corrupt, size, &cont);
    TASK_CHECK(cont.status[5] == -1 && cont.status[6] == -1);

    /* seek to frame 9: after the 2-frame pre-roll shift, backtracking starts at
       index 7 and crosses bad frames 6 then 5 (two untrusted frames in a row)
       before the 511-byte window is exhausted. Forward decode restarts at 5 and
       must match the continuous timeline. */
    mp3dec_ex_t dec;
    TASK_CHECK(mp3dec_ex_open_buf(&dec, corrupt, size, MP3D_SEEK_TO_SAMPLE) == 0);
    const int per_frame = 2304;
    TASK_CHECK(mp3dec_ex_seek(&dec, 9u * per_frame) == 0);
    mp3d_sample_t *fbuf = NULL;
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    size_t n = mp3dec_ex_read_frame(&dec, &fbuf, &info, per_frame);
    TASK_CHECK(n == (size_t)per_frame && info.crc_status == 1);
    int j, maxdiff = 0;
    const mp3d_sample_t *ref = cont.pcm + 9 * per_frame;
    for (j = 0; j < per_frame; j++)
    {
        int d = abs((int)fbuf[j] - (int)ref[j]);
        if (d > maxdiff) maxdiff = d;
    }
    TASK_CHECK(maxdiff <= 4);
    TASK_CHECK(dec.last_error == 0); /* mismatch never becomes a fatal stream error */
    (void)nframes;
    mp3dec_ex_close(&dec);
    free(corrupt);
    free(buf);
    return 0;
}

static int run_default_mode_reference(void)
{
    uint8_t *buf; size_t size;
    TASK_CHECK(load_file(VECTOR_PATH, &buf, &size) == 0);
    struct frame_desc frames[64];
    int nframes = build_frame_table(buf, size, frames, 64);
    const int bad = 5;
    uint8_t *corrupt = (uint8_t*)malloc(size);
    memcpy(corrupt, buf, size);
    corrupt[frames[bad].off + 4] ^= 0x01; /* stored CRC flip */

    /* the unmodified (CRC-less) reference build ignores protection entirely:
       default-mode behaviour is preserved and the corrupted frame still decodes */
    reference_mp3dec_t dec;
    reference_mp3dec_init(&dec);
    size_t off = 0;
    int k, frames_seen = 0, bad_nonzero = 0;
    for (k = 0; k < nframes; k++)
    {
        reference_mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int n = reference_mp3dec_decode_frame(&dec, corrupt + off, (int)(size - off), pcm, &info);
        TASK_CHECK(info.frame_bytes == frames[k].frame_bytes && n == frames[k].samples);
        if (k == bad) bad_nonzero = any_nonzero(pcm, n * info.channels);
        off += info.frame_bytes;
        frames_seen++;
    }
    TASK_CHECK(frames_seen == nframes && bad_nonzero);

    /* unprotected MPEG-2 material is untouched by the feature as well */
    uint8_t *m2; size_t m2size;
    TASK_CHECK(load_file(M2_PATH, &m2, &m2size) == 0);
    mp3dec_t pdec;
    mp3dec_init(&pdec);
    off = 0;
    while (off < m2size)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int n = mp3dec_decode_frame(&pdec, m2 + off, (int)(m2size - off), pcm, &info);
        if (!info.frame_bytes) break;
        TASK_CHECK(info.crc_status == 0);
        off += info.frame_bytes;
        (void)n;
    }
    TASK_CHECK(off > 0);
    free(m2);
    free(corrupt);
    free(buf);
    return 0;
}

/* ============================ registration ============================ */

const struct task_case task_cases[] = {
    { "protected", "status_basics",      run_status_basics },
    { "protected", "crc_bitwise",        run_crc_bitwise },
    { "protected", "scope_maindata",     run_scope_maindata },
    { "protected", "synth_structure",    run_synth_structure },
    { "protected", "other_layer_unchecked", run_other_layer_unchecked },
    { "recovery",  "good_bad_good",      run_good_bad_good },
    { "recovery",  "reservoir_payload",  run_reservoir_payload },
    { "recovery",  "consecutive_bad",    run_consecutive_bad },
    { "recovery",  "pseudo_sync",        run_pseudo_sync },
    { "recovery",  "truncated",          run_truncated },
    { "recovery",  "info_only",          run_info_only },
    { "extended",  "ex_buf_timeline",    run_ex_buf_timeline },
    { "extended",  "small_reads",        run_ex_small_reads },
    { "extended",  "buf_callback_match", run_buf_callback_match },
    { "extended",  "seek_bad_and_good",  run_seek_bad_and_good },
    { "extended",  "seek_backward",      run_seek_backward },
    { "extended",  "seek_consecutive_bad", run_seek_consecutive_bad },
    { "extended",  "default_mode_reference", run_default_mode_reference },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
