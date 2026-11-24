/* Copyright (c) 2026 The Opus contributors
   Written for the multistream timeline regression suite */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Content regression for the Opus multistream decoder, in three groups:

   framing:  multistream packets are assembled by hand from single-stream
             encoder output (RFC 6716 section 3.2 and Appendix B) covering
             ToC codes 0/1/2/3, CBR and VBR frame bundles, one- and
             two-byte self-delimited sizes and code-3 padding.  Decoded
             output is compared sample-by-sample against independent
             single-stream decoders fed the plain packets.

   mapping:  streams=3, coupled=1 with mapping {2,1,255,0,3,2} exercises
             reordering, a duplicated channel and a muted (255) channel in
             both the float and the int16 API, each with its own decoder
             history.

   recovery: a loss timeline over in-band-FEC wideband material (warmup,
             one loss recovered from the next packet, two consecutive PLC
             frames, reset+replay against a fresh instance) plus a
             CELT-only check that an FEC request falls back to PLC.

   This file never calls the multistream encoder, the internal packet
   parser, or the multistream pad/unpad helpers; every packet is built
   from the frame array kept by the test itself.  See doc/ms-timeline.md. */

#include "task_support/check.h"
#include "opus.h"
#include "opus_multistream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_PI 3.14159265358979323846

#define FS 48000
#define FR 960              /* 20 ms per frame */
#define MAX_FRAMES 24
#define MAX_PAYLOAD 1275    /* largest size encodable in a 2-byte size field */
#define MAX_PACKET 4096
#define MAX_OUT (3*FR)      /* most samples one event delivers per channel */
#define NCH 6               /* largest output channel count used */
#define GUARD 16
#define GUARD_FLOAT 1.0e30f
#define GUARD_INT16 ((opus_int16)0x5A3C)

/* One encoded 20 ms frame: ToC byte plus payload, exactly as produced by a
   single-stream encoder. */
typedef struct {
    unsigned char toc;
    int len;                        /* payload bytes, excluding the ToC */
    unsigned char payload[MAX_PAYLOAD];
} frame_t;

typedef struct {
    frame_t fr[3][MAX_FRAMES];      /* [stream][frame]: stereo, mono, mono */
    int nframes;
} material_t;

/* ------------------------------------------------------------------ */
/* Deterministic source material.  Each encoded channel gets its own
   frequency, phase and amplitude modulation so that a channel mix-up
   cannot hide behind identical content. */

static double tone_sample(int ch, long i)
{
    static const double freq[4]  = {413.0, 587.0, 733.0, 919.0};
    static const double phase[4] = {0.0, 0.9, 1.7, 2.6};
    static const double amf[4]   = {2.0, 3.5, 5.0, 6.5};
    static const double amp[4]   = {0.30, 0.26, 0.22, 0.19};
    double t = (double)i/FS;
    double n = sin(12.9898*(double)i + 78.233*(double)ch)*43758.5453;
    double v;
    n -= floor(n);
    v = amp[ch]*(0.72 + 0.28*sin(2*MS_PI*amf[ch]*t))
        *sin(2*MS_PI*freq[ch]*t + phase[ch]);
    v += 0.35*amp[ch]*sin(2*MS_PI*(2.71*freq[ch])*t + 2.0*phase[ch]);
    v += 0.03*(n - 0.5);
    return v;
}

/* Voiced-speech-like material: a harmonic stack with vibrato and syllabic
   amplitude modulation, distinct per channel. */
static double speech_sample(int ch, long i)
{
    double t = (double)i/FS;
    double f0 = 105.0 + 13.0*ch + 8.0*sin(2*MS_PI*3.7*t + ch);
    double v = 0.0;
    int h;
    for (h = 1; h <= 7; h++)
        v += (0.30/h)*sin(2*MS_PI*f0*h*t + 0.6*ch + 0.4*h);
    v *= 0.55 + 0.45*sin(2*MS_PI*(2.0 + 0.7*ch)*t + 0.5*ch);
    return 0.6*v;
}

/* ------------------------------------------------------------------ */
/* Material generation with independent single-stream encoders only. */

static int encode_material(material_t *m, int nframes, int application,
                           int vbr, const int bitrate[3], int use_fec,
                           int speech)
{
    static const int chs[3] = {2, 1, 1};
    OpusEncoder *enc[3];
    unsigned char pkt[MAX_PACKET];
    float pcm[2*FR];
    int err = OPUS_OK;
    int ret = -1;
    int s, k, i, c, n;
    long base;
    enc[0] = enc[1] = enc[2] = NULL;
    if (nframes <= 0 || nframes > MAX_FRAMES) return -1;
    for (s = 0; s < 3; s++) {
        enc[s] = opus_encoder_create(FS, chs[s], application, &err);
        if (!enc[s] || err != OPUS_OK) goto out;
        if (opus_encoder_ctl(enc[s], OPUS_SET_VBR(vbr)) != OPUS_OK) goto out;
        if (opus_encoder_ctl(enc[s], OPUS_SET_BITRATE(bitrate[s])) != OPUS_OK) goto out;
        if (use_fec) {
            if (opus_encoder_ctl(enc[s], OPUS_SET_INBAND_FEC(1)) != OPUS_OK) goto out;
            if (opus_encoder_ctl(enc[s], OPUS_SET_PACKET_LOSS_PERC(30)) != OPUS_OK) goto out;
            if (opus_encoder_ctl(enc[s], OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)) != OPUS_OK) goto out;
        }
    }
    base = 0;
    for (k = 0; k < nframes; k++) {
        for (s = 0; s < 3; s++) {
            int ch0 = (s == 0) ? 0 : s+1;   /* stream 0 -> sig ch 0/1, then 2, 3 */
            for (i = 0; i < FR; i++) {
                for (c = 0; c < chs[s]; c++) {
                    double v = speech ? speech_sample(ch0+c, base+i)
                                      : tone_sample(ch0+c, base+i);
                    pcm[i*chs[s]+c] = (float)v;
                }
            }
            n = opus_encode_float(enc[s], pcm, FR, pkt, (opus_int32)sizeof pkt);
            if (n < 2 || n-1 > MAX_PAYLOAD) {
                fprintf(stderr, "MS_TIMELINE_ENCODE stream=%d frame=%d len=%d\n", s, k, n);
                goto out;
            }
            m->fr[s][k].toc = pkt[0];
            m->fr[s][k].len = n-1;
            memcpy(m->fr[s][k].payload, pkt+1, (size_t)(n-1));
        }
        base += FR;
    }
    m->nframes = nframes;
    ret = 0;
out:
    for (s = 0; s < 3; s++) if (enc[s]) opus_encoder_destroy(enc[s]);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Independent packet assembly, following RFC 6716 section 3.2 (frame
   packing) and Appendix B (self-delimited framing).  Durations come from
   the test's own ToC table and lengths from the kept frame array. */

/* Samples per frame at 48 kHz for the ToC byte's config, per RFC 6716
   section 3.1 Table 2. */
static int toc_frame_samples(unsigned char toc)
{
    int config = toc >> 3;
    if (config < 12) {                      /* SILK-only: 10/20/40/60 ms */
        static const int ms[4] = {10, 20, 40, 60};
        return (FS/1000)*ms[config & 3];
    }
    if (config < 16)                        /* hybrid: 10 or 20 ms */
        return (FS/1000)*((config & 1) ? 20 : 10);
    return (FS/400) << (config & 3);        /* CELT-only: 2.5/5/10/20 ms */
}

/* RFC 6716 section 3.2.1 size encoding: one byte below 252, two bytes
   otherwise.  Returns the number of bytes written. */
static int emit_size(unsigned char *out, int n)
{
    if (n < 252) {
        out[0] = (unsigned char)n;
        return 1;
    }
    out[0] = (unsigned char)(252 + ((n - 252) & 3));
    out[1] = (unsigned char)((n - 252) >> 2);
    return 2;
}

/* Code-3 padding length: each 255 byte accounts for 254 padding bytes. */
static int emit_padlen(unsigned char *out, int pad)
{
    int n = 0;
    while (pad >= 255) {
        out[n++] = 255;
        pad -= 254;
    }
    out[n++] = (unsigned char)pad;
    return n;
}

/* Assemble one Opus packet from count consecutive frames of one stream.
   code selects the ToC frame-count code (0..3); vbr selects explicit
   sizes for code 3; pad appends that many zero padding bytes (code 3
   only); self_delim adds the Appendix B extra size for the last frame.
   Returns the packet length, or -1 if the combination is invalid. */
static int build_packet(unsigned char *out, const frame_t *f, int count,
                        int code, int vbr, int pad, int self_delim)
{
    unsigned char *p = out;
    int i;
    if (count < 1 || count > 3) return -1;
    if (code == 0 && count != 1) return -1;
    if ((code == 1 || code == 2) && count != 2) return -1;
    if (code == 3 && count < 2) return -1;
    if (pad > 0 && code != 3) return -1;
    for (i = 0; i < count; i++)
        if (f[i].len < 1 || f[i].len > MAX_PAYLOAD) return -1;
    if (code == 1 || (code == 3 && !vbr)) {
        for (i = 1; i < count; i++)
            if (f[i].len != f[0].len) return -1;
    }
    *p++ = (unsigned char)((f[0].toc & 0xFC) | code);
    if (code == 2)
        p += emit_size(p, f[0].len);
    if (code == 3) {
        unsigned char ch = (unsigned char)count;
        if (vbr) ch |= 0x80;
        if (pad > 0) ch |= 0x40;
        *p++ = ch;
        if (pad > 0)
            p += emit_padlen(p, pad);
        if (vbr)
            for (i = 0; i < count-1; i++)
                p += emit_size(p, f[i].len);
    }
    if (self_delim)
        p += emit_size(p, f[count-1].len);
    for (i = 0; i < count; i++) {
        memcpy(p, f[i].payload, (size_t)f[i].len);
        p += f[i].len;
    }
    if (pad > 0) {
        memset(p, 0, (size_t)pad);
        p += pad;
    }
    return (int)(p - out);
}

/* Build one multistream packet for frames k..k+count-1 of every stream:
   streams 0 and 1 are rewritten to self-delimited form, stream 2 keeps
   the plain packet.  Also yields the plain per-stream packets used as
   reference input and the independently computed sample count. */
static int assemble_event(const material_t *m, int k, int count, int code,
                          int vbr, int pad,
                          unsigned char *mspkt, int *mslen,
                          unsigned char nrm[3][MAX_PACKET],
                          const unsigned char *rp[3], int rl[3], int *nsamp)
{
    unsigned char sd[2][MAX_PACKET];
    int s, j, l, off, ns, ns2;
    for (s = 0; s < 3; s++) {
        for (j = 1; j < count; j++)
            if ((m->fr[s][k+j].toc & 0xFC) != (m->fr[s][k].toc & 0xFC)) return -1;
    }
    ns = 0;
    for (j = 0; j < count; j++) ns += toc_frame_samples(m->fr[0][k+j].toc);
    for (s = 1; s < 3; s++) {
        ns2 = 0;
        for (j = 0; j < count; j++) ns2 += toc_frame_samples(m->fr[s][k+j].toc);
        if (ns2 != ns) return -1;
    }
    for (s = 0; s < 3; s++) {
        l = build_packet(nrm[s], &m->fr[s][k], count, code, vbr, pad, 0);
        if (l <= 0) return -1;
        rp[s] = nrm[s];
        rl[s] = l;
    }
    off = 0;
    for (s = 0; s < 2; s++) {
        l = build_packet(sd[s], &m->fr[s][k], count, code, vbr, pad, 1);
        if (l <= 0) return -1;
        memcpy(mspkt+off, sd[s], (size_t)l);
        off += l;
    }
    memcpy(mspkt+off, nrm[2], (size_t)rl[2]);
    off += rl[2];
    *mslen = off;
    *nsamp = ns;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decode rig: one multistream decoder plus three independent
   single-stream reference decoders sharing one event timeline. */

typedef struct {
    OpusDecoder *ref[3];            /* stereo, mono, mono */
    OpusMSDecoder *ms;
    int nch;
    unsigned char map[NCH];
    long pos;                       /* start coordinate of the next delivery */
    double energy[3];               /* reference energy per stream */
} rig_t;

static int rig_create(rig_t *r, const unsigned char *map, int nch)
{
    static const int chs[3] = {2, 1, 1};
    int err = OPUS_OK;
    int s;
    memset(r, 0, sizeof *r);
    for (s = 0; s < 3; s++) {
        r->ref[s] = opus_decoder_create(FS, chs[s], &err);
        if (!r->ref[s] || err != OPUS_OK) return -1;
    }
    r->ms = opus_multistream_decoder_create(FS, nch, 3, 1, map, &err);
    if (!r->ms || err != OPUS_OK) return -1;
    r->nch = nch;
    memcpy(r->map, map, (size_t)nch);
    return 0;
}

static void rig_destroy(rig_t *r)
{
    int s;
    for (s = 0; s < 3; s++) if (r->ref[s]) opus_decoder_destroy(r->ref[s]);
    if (r->ms) opus_multistream_decoder_destroy(r->ms);
}

/* Expected sample for output channel c, sample i, expanded by hand from
   the mapping table: entries 0/1 are the coupled stream's L/R, entry
   idx>=2 is mono stream idx-1, 255 is digital silence. */
static float expected_sample(float *const ref[3], const unsigned char *map,
                             int c, int i, int *stream)
{
    int idx = map[c];
    if (idx == 255) {
        *stream = -1;
        return 0.0f;
    }
    if (idx < 2) {
        *stream = 0;
        return ref[0][2*i+idx];
    }
    *stream = idx-1;
    return ref[idx-1][i];
}

static int expand_compare_float(const char *group, int event,
                                const float *got, float *const ref[3],
                                const unsigned char *map, int nch, int nsamp,
                                double tol)
{
    int c, i;
    for (c = 0; c < nch; c++) {
        for (i = 0; i < nsamp; i++) {
            int stream;
            float want = expected_sample(ref, map, c, i, &stream);
            if (fabs((double)got[i*nch+c] - (double)want) > tol) {
                fprintf(stderr,
                        "MS_TIMELINE_DIFF group=%s event=%d stream=%d ch=%d sample=%d got=%.9g want=%.9g\n",
                        group, event, stream, c, i,
                        (double)got[i*nch+c], (double)want);
                TASK_CHECK(0);
            }
        }
    }
    return 0;
}

static int expand_compare_int16(const char *group, int event,
                                const opus_int16 *got, opus_int16 *const ref[3],
                                const unsigned char *map, int nch, int nsamp,
                                int tol)
{
    int c, i;
    for (c = 0; c < nch; c++) {
        for (i = 0; i < nsamp; i++) {
            int stream;
            int idx = map[c];
            int want;
            if (idx == 255) { stream = -1; want = 0; }
            else if (idx < 2) { stream = 0; want = ref[0][2*i+idx]; }
            else { stream = idx-1; want = ref[idx-1][i]; }
            if (abs((int)got[i*nch+c] - want) > tol) {
                fprintf(stderr,
                        "MS_TIMELINE_DIFF group=%s event=%d stream=%d ch=%d sample=%d got=%d want=%d\n",
                        group, event, stream, c, i,
                        (int)got[i*nch+c], want);
                TASK_CHECK(0);
            }
        }
    }
    return 0;
}

/* Deliver one event through the rig in the float API: pollute the output
   area, decode the multistream packet, check the sentinels on both sides,
   decode the three plain reference packets, expand the mapping by hand
   and compare.  A NULL/0 packet is a PLC delivery. */
static int run_event_float(rig_t *r, const char *group, int event,
                           const unsigned char *mspkt, int mslen,
                           const unsigned char *const pkt[3], const int len[3],
                           int nsamp, int fec, double tol,
                           float *save_ms, float *save_ref)
{
    float out[GUARD + NCH*MAX_OUT + GUARD];
    float refbuf[3][2*MAX_OUT];
    float *ref[3];
    int i, s, ret;
    ref[0] = refbuf[0];
    ref[1] = refbuf[1];
    ref[2] = refbuf[2];
    TASK_CHECK(nsamp > 0 && nsamp <= MAX_OUT);
    TASK_CHECK(r->nch > 0 && r->nch <= NCH);
    for (i = 0; i < GUARD + r->nch*nsamp + GUARD; i++) out[i] = GUARD_FLOAT;
    ret = opus_multistream_decode_float(r->ms, mspkt, mslen, out+GUARD, nsamp, fec);
    TASK_CHECK(ret == nsamp);
    for (i = 0; i < GUARD; i++) {
        if (out[i] != GUARD_FLOAT || out[GUARD + r->nch*nsamp + i] != GUARD_FLOAT) {
            fprintf(stderr, "MS_TIMELINE_GUARD group=%s event=%d slot=%d\n",
                    group, event, i);
            TASK_CHECK(0);
        }
    }
    for (s = 0; s < 3; s++) {
        ret = opus_decode_float(r->ref[s], pkt[s], len[s], ref[s], nsamp, fec);
        TASK_CHECK(ret == nsamp);
    }
    for (i = 0; i < nsamp; i++) {
        r->energy[0] += (double)ref[0][2*i]*ref[0][2*i]
                      + (double)ref[0][2*i+1]*ref[0][2*i+1];
        r->energy[1] += (double)ref[1][i]*ref[1][i];
        r->energy[2] += (double)ref[2][i]*ref[2][i];
    }
    if (expand_compare_float(group, event, out+GUARD, ref, r->map, r->nch,
                             nsamp, tol)) return 1;
    if (save_ms)
        memcpy(save_ms, out+GUARD, (size_t)r->nch*nsamp*sizeof(float));
    if (save_ref) {
        /* [stream0 L | stream0 R | stream1 | stream2], nsamp each */
        for (i = 0; i < nsamp; i++) {
            save_ref[i]         = ref[0][2*i];
            save_ref[nsamp+i]   = ref[0][2*i+1];
            save_ref[2*nsamp+i] = ref[1][i];
            save_ref[3*nsamp+i] = ref[2][i];
        }
    }
    r->pos += nsamp;
    return 0;
}

static int run_event_int16(rig_t *r, const char *group, int event,
                           const unsigned char *mspkt, int mslen,
                           const unsigned char *const pkt[3], const int len[3],
                           int nsamp, int tol, opus_int16 *save_ms)
{
    opus_int16 out[GUARD + NCH*MAX_OUT + GUARD];
    opus_int16 refbuf[3][2*MAX_OUT];
    opus_int16 *ref[3];
    int i, s, ret;
    ref[0] = refbuf[0];
    ref[1] = refbuf[1];
    ref[2] = refbuf[2];
    TASK_CHECK(nsamp > 0 && nsamp <= MAX_OUT);
    for (i = 0; i < GUARD + r->nch*nsamp + GUARD; i++) out[i] = GUARD_INT16;
    ret = opus_multistream_decode(r->ms, mspkt, mslen, out+GUARD, nsamp, 0);
    TASK_CHECK(ret == nsamp);
    for (i = 0; i < GUARD; i++) {
        if (out[i] != GUARD_INT16 || out[GUARD + r->nch*nsamp + i] != GUARD_INT16) {
            fprintf(stderr, "MS_TIMELINE_GUARD group=%s event=%d slot=%d\n",
                    group, event, i);
            TASK_CHECK(0);
        }
    }
    for (s = 0; s < 3; s++) {
        ret = opus_decode(r->ref[s], pkt[s], len[s], ref[s], nsamp, 0);
        TASK_CHECK(ret == nsamp);
    }
    if (expand_compare_int16(group, event, out+GUARD, ref, r->map, r->nch,
                             nsamp, tol)) return 1;
    if (save_ms)
        memcpy(save_ms, out+GUARD, (size_t)r->nch*nsamp*sizeof(opus_int16));
    r->pos += nsamp;
    return 0;
}

/* One framing-group event: assemble the multistream packet and the plain
   reference packets for frames k..k+count-1 and deliver them. */
static int framing_event(rig_t *r, const char *group, int ev,
                         const material_t *m, int k, int count, int code,
                         int vbr, int pad, double tol, float *save)
{
    unsigned char nrm[3][MAX_PACKET];
    unsigned char mspkt[3*MAX_PACKET];
    const unsigned char *rp[3];
    int rl[3];
    int msl = 0;
    int nsamp = 0;
    TASK_CHECK(assemble_event(m, k, count, code, vbr, pad,
                              mspkt, &msl, nrm, rp, rl, &nsamp) == 0);
    return run_event_float(r, group, ev, mspkt, msl, rp, rl, nsamp, 0, tol,
                           save, NULL);
}

/* One recovery-group event: idx is the packet index to deliver, or -1 for
   a PLC delivery; fec requests in-band FEC from packet idx. */
static int voice_event(rig_t *r, const char *group, int ev,
                       const material_t *m, int idx, int fec,
                       float *save_ms, float *save_ref)
{
    unsigned char nrm[3][MAX_PACKET];
    unsigned char mspkt[3*MAX_PACKET];
    const unsigned char *rp[3];
    int rl[3];
    int msl = 0;
    int nsamp = 0;
    if (idx < 0) {
        rp[0] = rp[1] = rp[2] = NULL;
        rl[0] = rl[1] = rl[2] = 0;
        return run_event_float(r, group, ev, NULL, 0, rp, rl, FR, 0, 0.0,
                               save_ms, save_ref);
    }
    TASK_CHECK(assemble_event(m, idx, 1, 0, 0, 0,
                              mspkt, &msl, nrm, rp, rl, &nsamp) == 0);
    TASK_CHECK(nsamp == FR);
    return run_event_float(r, group, ev, mspkt, msl, rp, rl, FR, fec, 0.0,
                           save_ms, save_ref);
}

/* Largest per-stream squared difference between two saved reference
   layouts ([s0 L | s0 R | s1 | s2], nsamp each). */
static double max_stream_diff(const float *a, const float *b, int nsamp)
{
    double d[3] = {0.0, 0.0, 0.0};
    double best;
    int i;
    for (i = 0; i < nsamp; i++) {
        double x;
        x = (double)a[i] - b[i];                 d[0] += x*x;
        x = (double)a[nsamp+i] - b[nsamp+i];     d[0] += x*x;
        x = (double)a[2*nsamp+i] - b[2*nsamp+i]; d[1] += x*x;
        x = (double)a[3*nsamp+i] - b[3*nsamp+i]; d[2] += x*x;
    }
    best = d[0];
    if (d[1] > best) best = d[1];
    if (d[2] > best) best = d[2];
    return best;
}

static double interleaved_diff(const float *a, const float *b, int n)
{
    double d = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double x = (double)a[i] - b[i];
        d += x*x;
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* framing group */

static int case_framing_cbr(void)
{
    static const unsigned char map[4] = {0, 1, 2, 3};
    /* {start frame, frame count, ToC code, vbr, padding} */
    static const int script[][5] = {
        {0, 1, 0, 0, 0},      /* single frame */
        {1, 1, 0, 0, 0},
        {2, 2, 1, 0, 0},      /* two equal CBR frames */
        {4, 3, 3, 0, 7},      /* code 3 CBR, one-byte padding length */
        {7, 1, 0, 0, 0},
        {8, 3, 3, 0, 300},    /* code 3 CBR, multi-byte padding length */
    };
    material_t m;
    int br[3] = {132000, 64000, 48000};
    unsigned char tmp[2];
    rig_t r;
    long tot[3] = {0, 0, 0};
    int s, k, ev, row;
    TASK_CHECK(encode_material(&m, 11, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               0, br, 0, 0) == 0);
    /* The raw encoder packets must be genuine single-frame audio packets. */
    for (s = 0; s < 3; s++) {
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
            if (k > 0)
                TASK_CHECK((m.fr[s][k].toc & 0xFC) == (m.fr[s][0].toc & 0xFC));
        }
    }
    /* CBR: every frame of a stream has the same byte count. */
    for (s = 0; s < 3; s++)
        for (k = 1; k < m.nframes; k++)
            TASK_CHECK(m.fr[s][k].len == m.fr[s][0].len);
    /* The stereo stream needs two-byte self-delimited sizes, the mono
       streams one-byte sizes. */
    TASK_CHECK(m.fr[0][0].len >= 252);
    TASK_CHECK(emit_size(tmp, m.fr[0][0].len) == 2);
    TASK_CHECK(emit_size(tmp, m.fr[1][0].len) == 1);
    TASK_CHECK(emit_size(tmp, m.fr[2][0].len) == 1);
    /* Per-stream totals from the test's own ToC table must agree. */
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++)
            tot[s] += toc_frame_samples(m.fr[s][k].toc);
    TASK_CHECK(tot[0] == tot[1] && tot[1] == tot[2]);
    TASK_CHECK(tot[0] == (long)m.nframes*FR);
    TASK_CHECK(rig_create(&r, map, 4) == 0);
    ev = 0;
    for (row = 0; row < 6; row++) {
        if (framing_event(&r, "framing", ev, &m, script[row][0],
                          script[row][1], script[row][2], script[row][3],
                          script[row][4], 0.0, NULL)) return 1;
        ev++;
    }
    TASK_CHECK(r.pos == tot[0]);
    for (s = 0; s < 3; s++)
        TASK_CHECK(r.energy[s] > 10.0);
    rig_destroy(&r);
    return 0;
}

static int case_framing_vbr(void)
{
    static const unsigned char map[4] = {0, 1, 2, 3};
    material_t m;
    int br[3] = {96000, 48000, 40000};
    rig_t r;
    long tot[3] = {0, 0, 0};
    int s, k, ev, pair, triple;
    TASK_CHECK(encode_material(&m, 14, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               1, br, 0, 0) == 0);
    for (s = 0; s < 3; s++) {
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
            if (k > 0)
                TASK_CHECK((m.fr[s][k].toc & 0xFC) == (m.fr[s][0].toc & 0xFC));
        }
    }
    /* Locate a genuinely variable-length adjacent pair for ToC code 2 and
       a varying triple for code 3 VBR. */
    pair = -1;
    for (k = 1; k+1 < m.nframes && pair < 0; k++) {
        for (s = 0; s < 3; s++) {
            if (m.fr[s][k].len != m.fr[s][k+1].len) {
                pair = k;
                break;
            }
        }
    }
    TASK_CHECK(pair > 0);
    triple = -1;
    for (k = pair+2; k+2 < m.nframes && triple < 0; k++) {
        for (s = 0; s < 3; s++) {
            if (m.fr[s][k].len != m.fr[s][k+1].len
                || m.fr[s][k+1].len != m.fr[s][k+2].len) {
                triple = k;
                break;
            }
        }
    }
    TASK_CHECK(triple > pair+1);
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++)
            tot[s] += toc_frame_samples(m.fr[s][k].toc);
    TASK_CHECK(tot[0] == tot[1] && tot[1] == tot[2]);
    TASK_CHECK(tot[0] == (long)m.nframes*FR);
    TASK_CHECK(rig_create(&r, map, 4) == 0);
    ev = 0;
    k = 0;
    while (k < m.nframes) {
        int count = 1, code = 0, vbr = 0, pad = 0;
        if (k == pair) {
            count = 2; code = 2;             /* two VBR frames, explicit size */
        } else if (k == triple) {
            count = 3; code = 3; vbr = 1; pad = 37;   /* code 3 VBR + padding */
        }
        if (framing_event(&r, "framing", ev, &m, k, count, code, vbr, pad,
                          0.0, NULL)) return 1;
        k += count;
        ev++;
    }
    TASK_CHECK(r.pos == tot[0]);
    for (s = 0; s < 3; s++)
        TASK_CHECK(r.energy[s] > 10.0);
    rig_destroy(&r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mapping group: streams=3, coupled=1, map {2,1,255,0,3,2}
   out0 = stream1 (mono), out1 = stream0 R, out2 = muted,
   out3 = stream0 L, out4 = stream2 (mono), out5 = stream1 again. */

static int case_mapping_float(void)
{
    static const unsigned char map[6] = {2, 1, 255, 0, 3, 2};
    material_t m;
    int br[3] = {120000, 60000, 44000};
    float save[NCH*FR];
    double energy[NCH] = {0, 0, 0, 0, 0, 0};
    double lr_diff = 0.0;
    rig_t r;
    int s, k, i, c;
    TASK_CHECK(encode_material(&m, 8, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               0, br, 0, 0) == 0);
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
        }
    TASK_CHECK(rig_create(&r, map, 6) == 0);
    for (k = 0; k < m.nframes; k++) {
        if (framing_event(&r, "mapping", k, &m, k, 1, 0, 0, 0, 2e-6,
                          save)) return 1;
        for (i = 0; i < FR; i++) {
            double lr;
            for (c = 0; c < 6; c++)
                energy[c] += (double)save[i*6+c]*save[i*6+c];
            lr = (double)save[i*6+1] - save[i*6+3];
            lr_diff += lr*lr;
            /* The duplicated channel must be an exact copy. */
            TASK_CHECK(save[i*6+0] == save[i*6+5]);
            /* The 255 channel must be digital silence. */
            TASK_CHECK(save[i*6+2] == 0.0f);
        }
    }
    TASK_CHECK(r.pos == (long)m.nframes*FR);
    /* Audible channels carry real signal and left really differs from
       right; equality of the duplicated channel alone proves nothing. */
    for (c = 0; c < 6; c++) {
        if (c == 2) continue;
        TASK_CHECK(energy[c] > 1.0);
    }
    TASK_CHECK(energy[2] == 0.0);
    TASK_CHECK(lr_diff > 1.0);
    rig_destroy(&r);
    return 0;
}

static int case_mapping_int16(void)
{
    static const unsigned char map[6] = {2, 1, 255, 0, 3, 2};
    material_t m;
    int br[3] = {120000, 60000, 44000};
    opus_int16 save[NCH*FR];
    unsigned char nrm[3][MAX_PACKET];
    unsigned char mspkt[3*MAX_PACKET];
    const unsigned char *rp[3];
    int rl[3];
    double energy[NCH] = {0, 0, 0, 0, 0, 0};
    double lr_diff = 0.0;
    rig_t r;
    int s, k, i, c, msl, nsamp;
    TASK_CHECK(encode_material(&m, 8, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               0, br, 0, 0) == 0);
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
        }
    TASK_CHECK(rig_create(&r, map, 6) == 0);
    for (k = 0; k < m.nframes; k++) {
        msl = 0;
        nsamp = 0;
        TASK_CHECK(assemble_event(&m, k, 1, 0, 0, 0,
                                  mspkt, &msl, nrm, rp, rl, &nsamp) == 0);
        TASK_CHECK(nsamp == FR);
        if (run_event_int16(&r, "mapping", k, mspkt, msl, rp, rl, FR, 1,
                            save)) return 1;
        for (i = 0; i < FR; i++) {
            double lr;
            for (c = 0; c < 6; c++) {
                double v = save[i*6+c];
                energy[c] += v*v;
            }
            lr = (double)save[i*6+1] - save[i*6+3];
            lr_diff += lr*lr;
            TASK_CHECK(save[i*6+0] == save[i*6+5]);
            TASK_CHECK(save[i*6+2] == 0);
        }
    }
    TASK_CHECK(r.pos == (long)m.nframes*FR);
    for (c = 0; c < 6; c++) {
        if (c == 2) continue;
        TASK_CHECK(energy[c] > 1.0e6);
    }
    TASK_CHECK(energy[2] == 0.0);
    TASK_CHECK(lr_diff > 1.0e6);
    rig_destroy(&r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* recovery group */

/* The shared 24-packet loss timeline: eight warmup packets, packet 8 lost
   (recovered from packet 9 with decode_fec=1 when fec_at_8 is set,
   otherwise concealed by PLC), packet 9 then decoded normally, and two
   consecutive PLC deliveries at events 14/15 before playback resumes.
   Every delivery starts at coordinate event*FR; the FEC recovery and the
   normal decode of the carrier packet advance the clock by one frame
   each, never two. */
static int voice_pass(rig_t *r, const material_t *m, int fec_at_8,
                      float *save_ms, float *save_ref)
{
    int ev;
    for (ev = 0; ev < 24; ev++) {
        int idx = ev;
        int fec = 0;
        float *sm = save_ms ? save_ms + (size_t)ev*4*FR : NULL;
        float *sr = save_ref ? save_ref + (size_t)ev*4*FR : NULL;
        TASK_CHECK(r->pos == (long)ev*FR);
        if (ev == 8) {
            if (fec_at_8) {
                idx = 9;
                fec = 1;
            } else {
                idx = -1;
            }
        } else if (ev == 14 || ev == 15) {
            idx = -1;
        }
        if (voice_event(r, "recovery", ev, m, idx, fec, sm, sr)) return 1;
    }
    TASK_CHECK(r->pos == 24L*FR);
    return 0;
}

static int case_recovery_fec(void)
{
    static const unsigned char map[4] = {0, 1, 2, 3};
    static float ref_main[24][4*FR];
    static float ref_plc[24][4*FR];
    static float ms_reset[24][4*FR];
    static float ms_fresh[24][4*FR];
    material_t m;
    int br[3] = {40000, 20000, 20000};
    rig_t r, plc, fresh;
    int s, k, ev, i;
    TASK_CHECK(encode_material(&m, 24, OPUS_APPLICATION_VOIP, 1, br, 1,
                               1) == 0);
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
        }
    TASK_CHECK(rig_create(&r, map, 4) == 0);
    TASK_CHECK(rig_create(&plc, map, 4) == 0);
    /* Main timeline: packet 8 recovered through in-band FEC. */
    if (voice_pass(&r, &m, 1, NULL, &ref_main[0][0])) return 1;
    /* Parallel timeline with identical history except packet 8 is
       concealed by PLC instead. */
    if (voice_pass(&plc, &m, 0, NULL, &ref_plc[0][0])) return 1;
    /* The configured FEC must really carry redundant audio: on at least
       one stream the recovered frame differs both from same-history PLC
       and from decoding the carrier packet 9 as a normal packet. */
    TASK_CHECK(max_stream_diff(ref_main[8], ref_plc[8], FR) > 1e-3);
    TASK_CHECK(max_stream_diff(ref_main[8], ref_main[9], FR) > 1e-3);
    /* Reset every decoder and replay the whole timeline; a fresh instance
       with the same configuration must produce identical output. */
    TASK_CHECK(opus_multistream_decoder_ctl(r.ms, OPUS_RESET_STATE) == OPUS_OK);
    for (s = 0; s < 3; s++)
        TASK_CHECK(opus_decoder_ctl(r.ref[s], OPUS_RESET_STATE) == OPUS_OK);
    r.pos = 0;
    TASK_CHECK(rig_create(&fresh, map, 4) == 0);
    if (voice_pass(&r, &m, 1, &ms_reset[0][0], NULL)) return 1;
    if (voice_pass(&fresh, &m, 1, &ms_fresh[0][0], NULL)) return 1;
    for (ev = 0; ev < 24; ev++) {
        for (i = 0; i < 4*FR; i++) {
            if (ms_reset[ev][i] != ms_fresh[ev][i]) {
                fprintf(stderr,
                        "MS_TIMELINE_REPLAY group=recovery event=%d ch=%d sample=%d reset=%.9g fresh=%.9g\n",
                        ev, i % 4, i / 4,
                        (double)ms_reset[ev][i], (double)ms_fresh[ev][i]);
                TASK_CHECK(0);
            }
        }
    }
    rig_destroy(&r);
    rig_destroy(&plc);
    rig_destroy(&fresh);
    return 0;
}

static int case_recovery_celt(void)
{
    static const unsigned char map[4] = {0, 1, 2, 3};
    static float ms_main[16][4*FR];
    static float ms_plc[16][4*FR];
    material_t m;
    int br[3] = {128000, 64000, 64000};
    rig_t r, plc;
    int s, k, ev, i;
    TASK_CHECK(encode_material(&m, 16, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               0, br, 0, 0) == 0);
    for (s = 0; s < 3; s++)
        for (k = 0; k < m.nframes; k++) {
            TASK_CHECK((m.fr[s][k].toc & 3) == 0);
            TASK_CHECK(m.fr[s][k].len >= 20);
        }
    TASK_CHECK(rig_create(&r, map, 4) == 0);
    TASK_CHECK(rig_create(&plc, map, 4) == 0);
    for (ev = 0; ev < 16; ev++) {
        TASK_CHECK(r.pos == (long)ev*FR);
        TASK_CHECK(plc.pos == (long)ev*FR);
        if (ev == 8) {
            /* Packet 8 is lost.  CELT-only packets carry no LBRR, so
               asking packet 9 for FEC must fall back to plain PLC. */
            if (voice_event(&r, "recovery", ev, &m, 9, 1,
                            ms_main[ev], NULL)) return 1;
            if (voice_event(&plc, "recovery", ev, &m, -1, 0,
                            ms_plc[ev], NULL)) return 1;
        } else {
            if (voice_event(&r, "recovery", ev, &m, ev, 0,
                            ms_main[ev], NULL)) return 1;
            if (voice_event(&plc, "recovery", ev, &m, ev, 0,
                            ms_plc[ev], NULL)) return 1;
        }
    }
    TASK_CHECK(r.pos == 16L*FR);
    TASK_CHECK(plc.pos == 16L*FR);
    /* The FEC request must not have decoded packet 9 as normal audio. */
    TASK_CHECK(interleaved_diff(ms_main[8], ms_main[9], 4*FR) > 1e-3);
    /* The fallback must equal true PLC sample-exactly, and the continued
       playback history after the loss must match as well. */
    for (ev = 8; ev < 16; ev++) {
        for (i = 0; i < 4*FR; i++) {
            if (ms_main[ev][i] != ms_plc[ev][i]) {
                fprintf(stderr,
                        "MS_TIMELINE_PLC group=recovery event=%d ch=%d sample=%d fec_fallback=%.9g plc=%.9g\n",
                        ev, i % 4, i / 4,
                        (double)ms_main[ev][i], (double)ms_plc[ev][i]);
                TASK_CHECK(0);
            }
        }
    }
    rig_destroy(&r);
    rig_destroy(&plc);
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"framing",  "cbr_codes",    case_framing_cbr},
    {"framing",  "vbr_codes",    case_framing_vbr},
    {"mapping",  "float_layout", case_mapping_float},
    {"mapping",  "int16_layout", case_mapping_int16},
    {"recovery", "voice_fec",    case_recovery_fec},
    {"recovery", "celt_fec_plc", case_recovery_celt}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
