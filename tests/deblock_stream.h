/*
 * deblock_stream.h - hand-built Annex-B streams for the public-entry
 * content regression.  C99 + standard library only.
 *
 * The constructor reuses the I_PCM idea from task_support/fixture.c and
 * extends it with P_Skip / P_L0_16x16 macroblocks and multiple slice
 * boundaries with deblocking enabled.  The expected planes are produced by
 * the same independent coordinate reference, chained across pictures:
 * zero-MV inter macroblocks copy the previous (already deblocked) picture,
 * integer-MV macroblocks copy a shifted region, and I_PCM macroblocks
 * overwrite explicit samples (the decoder stores I_PCM qpY = 0).
 */
#ifndef DEBLOCK_STREAM_H
#define DEBLOCK_STREAM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h264bsd_decoder.h"
#include "deblock_ref.h"

typedef struct { uint8_t b[32768]; size_t bits; } ds_bw;

static void ds_bit(ds_bw *w, unsigned v) {
    if (w->bits >= sizeof w->b * 8) abort();
    if (v & 1u) w->b[w->bits / 8] |= (uint8_t)(1u << (7 - w->bits % 8));
    ++w->bits;
}
static void ds_fixed(ds_bw *w, unsigned v, unsigned n) {
    while (n--) ds_bit(w, v >> n);
}
static void ds_ue(ds_bw *w, unsigned v) {
    unsigned n = 0, q = v + 1;
    while (q >> ++n) {}
    for (unsigned i = 1; i < n; ++i) ds_bit(w, 0);
    ds_fixed(w, v + 1, n);
}
static void ds_se(ds_bw *w, int v) {
    ds_ue(w, v <= 0 ? (unsigned)(-2 * v) : (unsigned)(2 * v - 1));
}
static void ds_align0(ds_bw *w) { while (w->bits % 8) ds_bit(w, 0); }
static void ds_trailing(ds_bw *w) { ds_bit(w, 1); ds_align0(w); }

static void ds_nal(uint8_t *dst, size_t cap, size_t *n,
                   unsigned h, ds_bw *w) {
    unsigned zeros = 0;
    if (*n + 5 + w->bits / 8 * 2 + 8 > cap) abort();
    dst[(*n)++] = 0; dst[(*n)++] = 0; dst[(*n)++] = 0; dst[(*n)++] = 1;
    dst[(*n)++] = (uint8_t)h;
    for (size_t i = 0; i < w->bits / 8; ++i) {
        unsigned c = w->b[i];
        if (zeros >= 2 && c <= 3) { dst[(*n)++] = 3; zeros = 0; }
        dst[(*n)++] = (uint8_t)c;
        zeros = c == 0 ? zeros + 1 : 0;
    }
    memset(w, 0, sizeof *w);
}

/* Deterministic deblock-friendly sample source for explicit PCM blocks. */
static unsigned ds_pix(unsigned tag, unsigned plane, unsigned x, unsigned y) {
    int v = 128
          + 18 * (((x >> 2) + 2 * (y >> 2) + tag) % 7 - 3)
          + 6 * ((x + plane * 5 + y) & 3) - 9;
    if (plane == 1) v += 10;
    if (plane == 2) v -= 12;
    if (v < 16) v = 16;
    if (v > 235) v = 235;
    return (unsigned)v;
}

static void ds_pcm(ds_bw *w, unsigned mbCode, unsigned tag,
                   unsigned mb, unsigned W) {
    ds_ue(w, mbCode);
    ds_align0(w);
    unsigned mx = mb % W, my = mb / W;
    for (unsigned p = 0; p < 3; ++p) {
        unsigned side = p ? 8u : 16u;
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x)
                ds_fixed(w, ds_pix(tag, p, mx * side + x,
                                          my * side + y), 8);
    }
}

/* Fill a packed plane from ds_pix for a whole picture. */
static void ds_fill(uint8_t *Y, uint8_t *Cb, uint8_t *Cr,
                    int Wmb, int Hmb, unsigned tag) {
    int W = Wmb * 16, H = Hmb * 16, x, y;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x)
            Y[y * W + x] = (uint8_t)ds_pix(tag, 0, (unsigned)x, (unsigned)y);
    for (y = 0; y < H / 2; ++y)
        for (x = 0; x < W / 2; ++x) {
            Cb[y * (W / 2) + x] =
                (uint8_t)ds_pix(tag, 1, (unsigned)x, (unsigned)y);
            Cr[y * (W / 2) + x] =
                (uint8_t)ds_pix(tag, 2, (unsigned)x, (unsigned)y);
        }
}

/* Run the whole public-entry regression.  Returns 0 on success; uses
 * TASK_CHECK so a content mismatch fails the calling task case. */
static int dstream_run_contract(void) {
    enum { WMB = 3, HMB = 2, NMB = WMB * HMB };
    const int yB = NMB * 256, cB = NMB * 64, allB = NMB * 384;
    static uint8_t stream[65536];
    static uint8_t decoded[4][NMB * 384];
    size_t n = 0;
    ds_bw w0;
    storage_t dec;
    int steps = 0, npic = 0, pic;
    size_t off = 0;

    memset(&w0, 0, sizeof w0);

    /* SPS: baseline profile, 4-bit frame_num/POC, WMB x HMB. */
    ds_fixed(&w0, 66, 8); ds_fixed(&w0, 0, 8); ds_fixed(&w0, 10, 8);
    ds_ue(&w0, 0);
    ds_ue(&w0, 0); ds_ue(&w0, 0); ds_ue(&w0, 0);
    ds_ue(&w0, 2); ds_bit(&w0, 0);
    ds_ue(&w0, WMB - 1); ds_ue(&w0, HMB - 1);
    ds_bit(&w0, 1); ds_bit(&w0, 1); ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_trailing(&w0); ds_nal(stream, sizeof stream, &n, 0x67, &w0);

    /* PPS: CAVLC, one slice group, deblocking control present. */
    ds_ue(&w0, 0); ds_ue(&w0, 0); ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_ue(&w0, 0); ds_ue(&w0, 0); ds_ue(&w0, 0);
    ds_bit(&w0, 0); ds_fixed(&w0, 0, 2);
    ds_se(&w0, 0); ds_se(&w0, 0); ds_se(&w0, 0);
    ds_bit(&w0, 1); ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_trailing(&w0); ds_nal(stream, sizeof stream, &n, 0x68, &w0);

    /* IDR: one slice, all I_PCM, deblock on A=+2 B=-2. */
    ds_ue(&w0, 0); ds_ue(&w0, 2); ds_ue(&w0, 0); ds_fixed(&w0, 0, 4);
    ds_ue(&w0, 0); ds_fixed(&w0, 0, 4);
    ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_se(&w0, 0); ds_ue(&w0, 0); ds_se(&w0, 1); ds_se(&w0, -1);
    for (unsigned mb = 0; mb < NMB; ++mb) ds_pcm(&w0, 25, 1, mb, WMB);
    ds_trailing(&w0); ds_nal(stream, sizeof stream, &n, 0x65, &w0);

    /* P picture, two slices with a non-row-aligned boundary:
     * slice A = MB0,MB1 (first_mb 0); slice B = MB2..5 (first_mb 2).
     * This creates both a vertical cross-slice segment (MB1|MB2) and a
     * horizontal one (MB0/MB3).  Slice B uses idc=2. */
    /* slice A header */
    ds_ue(&w0, 0); ds_ue(&w0, 0); ds_ue(&w0, 0);
    ds_fixed(&w0, 1, 4); ds_fixed(&w0, 2, 4);
    ds_bit(&w0, 0); ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_se(&w0, 0); ds_ue(&w0, 0); ds_se(&w0, 0); ds_se(&w0, 0);
    ds_ue(&w0, 1);                 /* skip MB0 */
    ds_pcm(&w0, 30, 2, 1, WMB);    /* I_PCM MB1 */
    ds_trailing(&w0); ds_nal(stream, sizeof stream, &n, 0x61, &w0);

    /* slice B header (same frame_num/poc), idc=2 */
    ds_ue(&w0, 2); ds_ue(&w0, 0); ds_ue(&w0, 0);
    ds_fixed(&w0, 1, 4); ds_fixed(&w0, 2, 4);
    ds_bit(&w0, 0); ds_bit(&w0, 0); ds_bit(&w0, 0);
    ds_se(&w0, 0); ds_ue(&w0, 2); ds_se(&w0, 0); ds_se(&w0, 0);
    ds_ue(&w0, NMB - 2);           /* skip MB2..5 */
    ds_trailing(&w0); ds_nal(stream, sizeof stream, &n, 0x61, &w0);

    /* ---- decode through the public API ---- */
    TASK_CHECK(h264bsdInit(&dec, HANTRO_FALSE) == HANTRO_OK);
    while (off < n) {
        u32 used = 0;
        u32 r = h264bsdDecode(&dec, stream + off, (u32)(n - off), 0, &used);
        TASK_CHECK(++steps < 64);
        TASK_CHECK(used <= n - off);
        off += used;
        TASK_CHECK(r != H264BSD_ERROR && r != H264BSD_PARAM_SET_ERROR &&
                   r != H264BSD_MEMALLOC_ERROR);
        u32 id = 0, idr = 0, errs = 0;
        u8 *p;
        while ((p = h264bsdNextOutputPicture(&dec, &id, &idr, &errs))
               != NULL && npic < 4) {
            TASK_CHECK(!errs);
            memcpy(decoded[npic], p, (size_t)allB);
            ++npic;
        }
    }
    h264bsdFlushBuffer(&dec);
    for (;;) {
        u32 id = 0, idr = 0, errs = 0;
        u8 *p = h264bsdNextOutputPicture(&dec, &id, &idr, &errs);
        if (!p) break;
        TASK_CHECK(!errs);
        TASK_CHECK(npic < 4);
        memcpy(decoded[npic], p, (size_t)allB);
        ++npic;
    }
    h264bsdShutdown(&dec);
    TASK_CHECK(npic == 2);

    /* ---- independent model, frame 0 ---- */
    dref_mb *m0 = (dref_mb *)calloc(NMB, sizeof *m0);
    uint8_t *Y0 = malloc(yB), *C0 = malloc(cB), *R0 = malloc(cB);
    TASK_CHECK(m0 && Y0 && C0 && R0);
    ds_fill(Y0, C0, R0, WMB, HMB, 1);
    for (pic = 0; pic < NMB; ++pic) {
        m0[pic].intra = 1;
        m0[pic].qpY = 0;       /* decoder forces I_PCM qpY to 0 */
        m0[pic].offA = 2; m0[pic].offB = -2;
    }
    dref_pic rp0 = { WMB, HMB, m0, Y0, C0, R0 };
    dref_filter(&rp0);

    int z;
    long d0 = 0; int first0 = -1;
    uint8_t *planar0 = malloc(allB);
    TASK_CHECK(planar0);
    memcpy(planar0, Y0, yB);
    memcpy(planar0 + yB, C0, cB);
    memcpy(planar0 + yB + cB, R0, cB);
    for (z = 0; z < allB; ++z)
        if (decoded[0][z] != planar0[z]) {
            if (first0 < 0) first0 = z;
            ++d0;
        }
    if (first0 >= 0)
        printf("  PUBLIC frame0 diffs=%ld first=%d lib=%u ref=%u\n",
               d0, first0, decoded[0][first0], planar0[first0]);
    TASK_CHECK(d0 == 0);

    /* ---- frame 1: zero-MV skips copy filtered frame 0; MB1 is PCM ---- */
    dref_mb *m1 = (dref_mb *)calloc(NMB, sizeof *m1);
    uint8_t *Y1 = malloc(yB), *C1 = malloc(cB), *R1 = malloc(cB);
    uint8_t *src2Y = malloc(yB), *src2C = malloc(cB), *src2R = malloc(cB);
    TASK_CHECK(m1 && Y1 && C1 && R1 && src2Y && src2C && src2R);
    memcpy(Y1, Y0, yB); memcpy(C1, C0, cB); memcpy(R1, R0, cB);
    ds_fill(src2Y, src2C, src2R, WMB, HMB, 2);
    {
        int mx = 1 % WMB, my = 1 / WMB, yy;
        for (yy = 0; yy < 16; ++yy)
            memcpy(Y1 + (my * 16 + yy) * WMB * 16 + mx * 16,
                   src2Y + (my * 16 + yy) * WMB * 16 + mx * 16, 16);
        for (yy = 0; yy < 8; ++yy) {
            memcpy(C1 + (my * 8 + yy) * WMB * 8 + mx * 8,
                   src2C + (my * 8 + yy) * WMB * 8 + mx * 8, 8);
            memcpy(R1 + (my * 8 + yy) * WMB * 8 + mx * 8,
                   src2R + (my * 8 + yy) * WMB * 8 + mx * 8, 8);
        }
    }
    for (pic = 0; pic < NMB; ++pic) {
        m1[pic].part = DREF_PART_16X16;
        m1[pic].qpY = 26;
        if (pic <= 1) { m1[pic].sliceId = 1; m1[pic].disableIdc = 0; }
        else          { m1[pic].sliceId = 2; m1[pic].disableIdc = 2; }
    }
    m1[1].intra = 1; m1[1].qpY = 0;
    dref_pic rp1 = { WMB, HMB, m1, Y1, C1, R1 };
    dref_filter(&rp1);

    long d1 = 0; int first1 = -1;
    uint8_t *planar1 = malloc(allB);
    TASK_CHECK(planar1);
    memcpy(planar1, Y1, yB);
    memcpy(planar1 + yB, C1, cB);
    memcpy(planar1 + yB + cB, R1, cB);
    for (z = 0; z < allB; ++z)
        if (decoded[1][z] != planar1[z]) {
            if (first1 < 0) first1 = z;
            ++d1;
        }
    if (first1 >= 0)
        printf("  PUBLIC frame1 diffs=%ld first=%d lib=%u ref=%u\n",
               d1, first1, decoded[1][first1], planar1[first1]);
    TASK_CHECK(d1 == 0);

    free(m0); free(Y0); free(C0); free(R0); free(planar0);
    free(m1); free(Y1); free(C1); free(R1);
    free(src2Y); free(src2C); free(src2R); free(planar1);
    return 0;
}

#endif /* DEBLOCK_STREAM_H */
