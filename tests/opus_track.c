/*
 * Opus-in-MP4 regression tests for task_support/check.h.
 *
 * Three groups:
 *   framing   - standalone packet validation (multi-stream delimiting,
 *               variable-length rejection, code3 padding and lengths)
 *   container - independent interpretation of the boxes we write
 *   demux     - hand-built MP4 materials read back through MP4D_*
 */
#include "check.h"
#include "minimp4.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

typedef struct
{
    unsigned char *data;
    long size;
    long cap;
} mem_file_t;

static int mem_write(int64_t offset, const void *buffer, size_t size, void *token)
{
    mem_file_t *f = (mem_file_t *)token;
    if (offset < 0)
        return 1;
    if (offset + (long)size > f->cap)
    {
        long cap = f->cap ? f->cap : 4096;
        unsigned char *p;
        while (cap < offset + (long)size)
            cap *= 2;
        p = (unsigned char *)realloc(f->data, (size_t)cap);
        if (!p)
            return 1;
        memset(p + f->cap, 0, (size_t)(cap - f->cap));
        f->data = p;
        f->cap = cap;
    }
    memcpy(f->data + offset, buffer, size);
    if (offset + (long)size > f->size)
        f->size = offset + (long)size;
    return 0;
}

static int mem_read(int64_t offset, void *buffer, size_t size, void *token)
{
    mem_file_t *f = (mem_file_t *)token;
    if (offset < 0 || offset >= f->size)
        return 1;
    if (offset + (long)size > f->size)
    {
        size_t avail = (size_t)(f->size - offset);
        memcpy(buffer, f->data + offset, avail);
        memset((unsigned char *)buffer + avail, 0, size - avail);
        return size != avail;
    }
    memcpy(buffer, f->data + offset, size);
    return 0;
}

static void mem_file_reset(mem_file_t *f)
{
    free(f->data);
    f->data = NULL;
    f->size = f->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Packet construction helpers                                         */
/* ------------------------------------------------------------------ */

/* RFC 6716 3.2.1: 0..251 one byte, 252..1275 two bytes. */
static int put_frame_length(unsigned char *p, unsigned len)
{
    if (len < 252)
    {
        p[0] = (unsigned char)len;
        return 1;
    }
    p[0] = (unsigned char)(252 + (len - 252) % 4);
    p[1] = (unsigned char)((len - 252) / 4);
    return 2;
}

/*
 * Build one stream packet. 'config' picks frame size; 'code' picks packing.
 * 'self_delim' adds the Appendix B extra length field.
 * 'payload_byte' fills frame bodies so different streams are distinguishable.
 * Returns packet size, or -1 on bad arguments.
 */
static int build_stream(unsigned char *out, unsigned config, unsigned code,
                        int self_delim, unsigned frame_len, unsigned frames,
                        unsigned char payload_byte, unsigned padding)
{
    unsigned char *p = out;
    unsigned f;

    *p++ = (unsigned char)((config << 3) | (code & 3));
    if (code == 0)
    {
        if (frames != 1 || frame_len > 1275)
            return -1;
        if (self_delim)
            p += put_frame_length(p, frame_len);
        for (f = 0; f < frame_len; f++)
            *p++ = (unsigned char)(payload_byte + f);
    } else if (code == 1)
    {
        if (frames != 2 || frame_len > 1275)
            return -1;
        if (self_delim)
            p += put_frame_length(p, frame_len);
        for (f = 0; f < 2*frame_len; f++)
            *p++ = (unsigned char)(payload_byte + f);
    } else if (code == 2)
    {
        if (frames != 2 || frame_len > 1275 || frame_len < 1)
            return -1;
        /* first length explicit, second inferred (only the first is coded) */
        p += put_frame_length(p, frame_len);
        if (self_delim)
            p += put_frame_length(p, frame_len);   /* length of the *second* frame */
        for (f = 0; f < 2*frame_len; f++)
            *p++ = (unsigned char)(payload_byte + f);
    } else
    {
        unsigned char *fc;
        if (!frames || frames > 48)
            return -1;
        fc = p;
        *p++ = 0;   /* v/p/M patched below */
        if (padding)
        {
            unsigned left = padding;
            *fc |= 0x40;
            while (left >= 255)
            {
                *p++ = 255;
                left -= 254;
            }
            *p++ = (unsigned char)left;
        }
        if (frames > 1)
        {
            *fc |= 0x80;    /* VBR */
            for (f = 0; f + 1 < frames; f++)
                p += put_frame_length(p, frame_len);
            if (self_delim)
                p += put_frame_length(p, frame_len);
        } else if (self_delim)
        {
            p += put_frame_length(p, frame_len);
        }
        *fc |= (unsigned char)frames;
        for (f = 0; f < frames*frame_len; f++)
            *p++ = (unsigned char)(payload_byte + f);
        for (f = 0; f < padding; f++)
            *p++ = 0;
    }
    return (int)(p - out);
}

/*
 * Assemble a full multi-stream sample: streams-1 self-delimited packets
 * followed by one regular packet. Each stream carries the same duration.
 */
static int build_multi_sample(unsigned char *out, int streams,
                              unsigned config, unsigned frame_len,
                              unsigned frames, unsigned char base_payload)
{
    unsigned char *p = out;
    int s;
    for (s = 0; s < streams; s++)
    {
        int n = build_stream(p, config, frames > 1 ? 3 : 0, s + 1 < streams,
                             frame_len, frames,
                             (unsigned char)(base_payload + 0x10*s), 0);
        if (n < 0)
            return -1;
        p += n;
    }
    return (int)(p - out);
}

/* ------------------------------------------------------------------ */
/* Box walking for the container group                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    const unsigned char *data;
    long size;
} span_t;

static int find_box(span_t parent, uint32_t name, span_t *child)
{
    long off = 0;
    while (off + 8 <= parent.size)
    {
        uint32_t size = be32(parent.data + off);
        uint32_t type = be32(parent.data + off + 4);
        long hdr = 8;
        if (size == 1)
        {
            if (off + 16 > parent.size)
                return 0;
            size = (uint32_t)(((uint64_t)be32(parent.data + off + 8) << 32) |
                              be32(parent.data + off + 12));
            hdr = 16;
        } else if (size == 0)
        {
            size = (uint32_t)(parent.size - off);
        }
        if (size < (uint32_t)hdr || off + (long)size > parent.size)
            return 0;
        if (type == name)
        {
            child->data = parent.data + off + hdr;
            child->size = (long)size - hdr;
            return 1;
        }
        off += (long)size;
    }
    return 0;
}

static int find_path(span_t parent, const uint32_t *names, int count, span_t *out)
{
    span_t cur = parent;
    int i;
    for (i = 0; i < count; i++)
    {
        if (!find_box(cur, names[i], &cur))
            return 0;
        if (i + 1 < count)
            continue;
    }
    *out = cur;
    return 1;
}

#define BOX4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ------------------------------------------------------------------ */
/* Framing group                                                       */
/* ------------------------------------------------------------------ */

/* Valid multi-stream sample is stored verbatim as one sample. */
static int case_framing_multi_stream(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0x78,0, 0x80,0xbb,0,0, 0,0, 255,
        2,1, 0,1
    };
    unsigned char sample[512];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int n, track;

    n = build_multi_sample(sample, 2, 0 /*10 ms silk*/, 4, 1, 0x40);
    TASK_CHECK(n > 0);

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, sample, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    /* The mdat must contain the input bytes untouched, exactly once. */
    {
        span_t root = { f.data, f.size }, mdat;
        long i, found = 0;
        TASK_CHECK(find_box(root, BOX4('m','d','a','t'), &mdat));
        TASK_CHECK(mdat.size == n);
        for (i = 0; i + n <= mdat.size; i++)
            if (!memcmp(mdat.data + i, sample, (size_t)n))
                found = 1;
        TASK_CHECK(found);
    }
    mem_file_reset(&f);
    return 0;
}

/* A stream whose frame lengths cannot delimit the packet is rejected. */
static int case_framing_bad_lengths(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0,0, 0,0,0,0, 0,0, 255, 2,0, 0,1
    };
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    unsigned char bad[64];
    int track, n;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);

    /* Two-stream sample: stream 0 claims a code-0 frame of 200 bytes but
       only a handful remain; stream 1 lies about its boundary too. */
    bad[0] = (unsigned char)(0 << 3);   /* 10 ms, code 0, self-delimited */
    {
        int off = 1;
        off += put_frame_length(bad + off, 200);
        bad[off++] = 0;
        n = off + 1;                    /* far fewer bytes than claimed */
        bad[off] = 0;                   /* last stream TOC */
    }
    TASK_CHECK(MP4E_put_opus_packet(mux, track, bad, n) != MP4E_STATUS_OK);

    /* An odd-sized code-1 regular stream is invalid per [R3]. */
    n = 0;
    bad[n++] = (unsigned char)(0 << 3);           /* stream 0: self-delimited code 0 */
    n += put_frame_length(bad + n, 1);
    bad[n++] = 0xAA;
    bad[n++] = (unsigned char)((0 << 3) | 1);     /* last stream: code 1 */
    bad[n++] = 0xBB;
    bad[n++] = 0xCC;
    bad[n++] = 0xDD;                              /* N-1 = 3, odd */
    TASK_CHECK(MP4E_put_opus_packet(mux, track, bad, n) != MP4E_STATUS_OK);

    /* After the rejects, a well-formed packet still works. */
    n = build_multi_sample(bad, 2, 0, 2, 1, 0x10);
    TASK_CHECK(n > 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, bad, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* Per-stream durations must agree; differing TOCs are allowed otherwise. */
static int case_framing_duration_mismatch(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0,0, 0,0,0,0, 0,0, 255, 2,0, 0,1
    };
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    unsigned char pkt[256];
    int track, n = 0;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);

    /* Stream 0: 10 ms. Stream 1: 20 ms. */
    n += build_stream(pkt + n, 0, 0, 1, 1, 1, 0x11, 0);
    n += build_stream(pkt + n, 1, 0, 0, 1, 1, 0x22, 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) != MP4E_STATUS_OK);

    /* Same 20 ms config on both: accepted even if payload/stereo differs. */
    n = 0;
    n += build_stream(pkt + n, 1, 0, 1, 1, 1, 0x11, 0);
    n += build_stream(pkt + n, 1, 0, 0, 1, 1, 0x22, 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* Code 3 padding, two-byte lengths, and the 120 ms packet limit. */
static int case_framing_code3(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0,0, 0,0,0,0, 0,0, 0,
    };
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    unsigned char pkt[4096];
    int track, n;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);

    /* A single CBR code-3 packet with 254 bytes of padding (single stream,
       so regular framing). */
    n = build_stream(pkt, 2 /*40 ms*/, 3, 0, 1, 1, 0x30, 254);
    TASK_CHECK(n > 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);

    /* 1275-byte frames need the two-byte length form. 3 x 40 ms is legal. */
    n = build_stream(pkt, 2, 3, 0, 1275, 3, 0x40, 0);
    TASK_CHECK(n > 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);

    /* 4 x 40 ms would be 160 ms and must be rejected. */
    {
        unsigned char big[8192];
        int m = build_stream(big, 2, 3, 0, 1, 4, 0x50, 0);
        TASK_CHECK(m > 0);
        TASK_CHECK(MP4E_put_opus_packet(mux, track, big, m) != MP4E_STATUS_OK);
    }

    /* A single 1276-byte frame cannot be represented (the maximum is
       1275). In the regular code-0 form the frame size is inferred from
       the packet length, so the limit is checked on that inferred size. */
    {
        unsigned char big[1300];
        int m = 0, i;
        big[m++] = 0;                       /* 10 ms, code 0, regular framing */
        for (i = 0; i < 1276; i++)
            big[m++] = 0x60;
        TASK_CHECK(MP4E_put_opus_packet(mux, track, big, m) != MP4E_STATUS_OK);
    }

    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* A rejected packet must not flush (or corrupt) a pending sequential one. */
static int case_framing_reject_keeps_sequential(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0,0, 0,0,0,0, 0,0, 255, 2,0, 0,1
    };
    unsigned char good[64], bad[16];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int track, n, bad_n = 0;

    n = build_multi_sample(good, 2, 0, 2, 1, 0x10);
    TASK_CHECK(n > 0);
    /* Stream 0 parses cleanly; the *last* stream is invalid per [R3]
       (odd code-1 payload). Validation must still complete before the
       pending sample is flushed. */
    bad[bad_n++] = 0x00;                 /* stream 0: 10 ms, code 0, self-delimited */
    bad[bad_n++] = 0x01;                 /* one payload byte */
    bad[bad_n++] = 0xAA;
    bad[bad_n++] = 0x01;                 /* last stream: code 1 */
    bad[bad_n++] = 0xBB;
    bad[bad_n++] = 0xCC;
    bad[bad_n++] = 0xDD;                 /* N-1 = 3 is odd => invalid */

    mux = MP4E_open(1, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, good, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, bad, bad_n) != MP4E_STATUS_OK);

    /* Recovery: a second legal packet must be accepted and the file kept
       readable with two samples. A malformed one must not have wiped the
       first packet's pending bytes. */
    TASK_CHECK(MP4E_put_opus_packet(mux, track, good, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    {
        MP4D_demux_t mp4;
        memset(&mp4, 0, sizeof(mp4));
        TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
        TASK_CHECK(mp4.track_count == 1);
        TASK_CHECK(mp4.track[0].sample_count == 2);
        /* The rejected packet must not have been flushed as a sample nor
           have consumed the first one's pending bytes: both stored samples
           match the accepted packet's length and content. */
        {
            unsigned i;
            for (i = 0; i < 2; i++)
            {
                unsigned bytes = 0, ts = 0, dur = 0;
                MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, i, &bytes, &ts, &dur);
                TASK_CHECK(bytes == (unsigned)n);
                TASK_CHECK(!memcmp(f.data + off, good, (size_t)n));
                TASK_CHECK(dur == 480);
            }
        }
        MP4D_close(&mp4);
    }
    mem_file_reset(&f);
    return 0;
}

/* OpusHead validation: rejected headers leave no half track behind. */
static int case_framing_head_validation(void)
{
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    unsigned char head[21 + 8];

    memcpy(head, "OpusHead", 8);
    head[8] = 1;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);

    /* Wrong magic */
    memset(head + 8, 0, sizeof(head) - 8);
    memcpy(head, "OpusHeaX", 8);
    head[8] = 1; head[9] = 1; head[18] = 0;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 19) < 0);

    /* Bad version */
    memcpy(head, "OpusHead", 8);
    head[8] = 2; head[9] = 1; head[18] = 0;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 19) < 0);

    /* Family 0 with 3 output channels */
    head[8] = 1; head[9] = 3; head[18] = 0;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 19) < 0);

    /* Family 0 with a longer-than-19-byte header */
    head[9] = 2;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 20) < 0);

    /* Reserved family */
    head[18] = 1;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 19) < 0);

    /* Family 255: truncated mapping table */
    head[9] = 2; head[18] = 255;
    head[19] = 2; head[20] = 0;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 21) < 0);

    /* Family 255: coupled > streams */
    head[19] = 1; head[20] = 2; head[21] = 0; head[22] = 1;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 23) < 0);

    /* Family 255: mapping index out of range */
    head[19] = 2; head[20] = 0; head[21] = 0; head[22] = 5;
    TASK_CHECK(MP4E_add_opus_track(mux, head, 23) < 0);

    /* Fragmentation mode is out of scope. */
    {
        mem_file_t g = {0};
        MP4E_mux_t *fm = MP4E_open(0, 1, &g, mem_write);
        head[19] = 2; head[20] = 0; head[21] = 0; head[22] = 1;
        TASK_CHECK(fm != NULL);
        TASK_CHECK(MP4E_add_opus_track(fm, head, 23) < 0);
        MP4E_close(fm);
        mem_file_reset(&g);
    }

    /* A valid header after all those rejects still gets track 0 (no
       half-allocated tracks preceding it). */
    TASK_CHECK(MP4E_add_opus_track(mux, head, 23) == 0);
    /* A track that was never finished still forces close() to report the
       missing seal, while releasing every resource. */
    TASK_CHECK(MP4E_close(mux) != MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* MP4E_put_sample must not accept samples on an Opus track. */
static int case_framing_put_sample_rejected(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0,0, 0,0,0,0, 0,0, 0
    };
    unsigned char sample[32];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int track, n;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    memset(sample, 0, sizeof(sample));
    TASK_CHECK(MP4E_put_sample(mux, track, sample, 16, 480, MP4E_SAMPLE_DEFAULT) != MP4E_STATUS_OK);

    n = build_multi_sample(sample, 1, 0, 1, 1, 0x10);
    TASK_CHECK(n > 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, sample, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* finish_opus enforces end_trim and its own one-shot semantics. */
static int case_framing_finish_rules(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0x78,0 /*120*/, 0,0,0,0, 0,0, 0
    };
    unsigned char sample[256];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int track, n = 0;

    /* 4 x 20 ms => 3840 frames total. */
    n += build_stream(sample + n, 1, 3, 0, 1, 4, 0x10, 0);
    TASK_CHECK(n > 0);

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);

    TASK_CHECK(MP4E_finish_opus(mux, track, 0) != MP4E_STATUS_OK);   /* no packets yet */

    TASK_CHECK(MP4E_put_opus_packet(mux, track, sample, n) == MP4E_STATUS_OK);

    /* end_trim must be smaller than the last packet's full duration. */
    TASK_CHECK(MP4E_finish_opus(mux, track, 3840) != MP4E_STATUS_OK);
    /* pre_skip + end_trim must fit into the accumulated duration. */
    TASK_CHECK(MP4E_finish_opus(mux, track, 3840) != MP4E_STATUS_OK);

    TASK_CHECK(MP4E_finish_opus(mux, track, 100) == MP4E_STATUS_OK);
    /* One shot */
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) != MP4E_STATUS_OK);
    /* No more packets */
    TASK_CHECK(MP4E_put_opus_packet(mux, track, sample, n) != MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);
    return 0;
}

/* close() refuses an Opus track that was never finished, then frees. */
static int case_framing_unfinished_close(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0,0, 0,0,0,0, 0,0, 0
    };
    unsigned char sample[64];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int track, n;

    n = build_multi_sample(sample, 1, 0, 1, 1, 0x10);
    TASK_CHECK(n > 0);
    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, sample, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) != MP4E_STATUS_OK);   // resources still released
    mem_file_reset(&f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container group: interpret the boxes we produce                     */
/* ------------------------------------------------------------------ */

/* Full featured file: 4 streams, coupled 2, 7 output channels. */
#define CONTAINER_HEAD_BYTES \
    'O','p','u','s','H','e','a','d', 1, 7, 0x38,0x01, 0x44,0xac,0,0, 0x01,0xff, 255, \
    4,2, 5,0,255,3,1,4,5

static const unsigned char container_head[] = { CONTAINER_HEAD_BYTES };
/* pre_skip = 0x0138 = 312, gain = 0xff01 = -255 */

/* Packet duration schedule: 2.5 ms, 20 ms, 60 ms, 20 ms, 20 ms (in 48k). */
static const unsigned container_durations[] = { 120, 960, 2880, 960, 960 };
static const unsigned container_configs[]   = { 16, 19, 3, 19, 19 };

static int build_container_file(mem_file_t *out, int sequential)
{
    MP4E_mux_t *mux;
    int track, i, ret = 0;
    unsigned char sample[2048];

    mux = MP4E_open(sequential, 0, out, mem_write);
    if (!mux)
        return 0;
    track = MP4E_add_opus_track(mux, container_head, (int)sizeof(container_head));
    if (track != 0)
        ret = 0;
    else for (i = 0; i < (int)ARRAY_LEN(container_durations); i++)
    {
        int n = build_multi_sample(sample, 4, container_configs[i], 2, 1,
                                   (unsigned char)(0x50 + i));
        if (n <= 0 || MP4E_put_opus_packet(mux, track, sample, n) != MP4E_STATUS_OK)
            ret = 0;
    }
    if (ret == 0)
        ret = MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK;
    if (MP4E_close(mux) != MP4E_STATUS_OK)
        ret = 0;
    return ret;
}

/* dOps fields: version, output channels, pre-skip, sample rate, gain, mapping. */
static int case_container_dops(void)
{
    mem_file_t f = {0};
    span_t root, stsd, entry, dops;
    static const uint32_t dops_path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                          BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                          BOX4('s','t','b','l'), BOX4('s','t','s','d') };
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_path(root, dops_path, ARRAY_LEN(dops_path), &stsd));
    /* stsd: 4 version/flags + 4 entry_count, then the sample entry */
    TASK_CHECK(stsd.size > 16);
    {
        span_t entries = { stsd.data + 8, stsd.size - 8 };
        TASK_CHECK(be32(stsd.data + 4) == 1);
        TASK_CHECK(find_box(entries, BOX4('O','p','u','s'), &entry));
    }
    {
        /* The sample entry payload starts with AudioSampleEntry fields;
           its child boxes follow the 28-byte fixed part. */
        span_t children = { entry.data + 28, entry.size - 28 };
        TASK_CHECK(find_box(children, BOX4('d','O','p','s'), &dops));
    }
    TASK_CHECK(dops.size == 13 + 7);
    TASK_CHECK(dops.data[0] == 0);           /* version */
    TASK_CHECK(dops.data[1] == 7);           /* OutputChannelCount */
    TASK_CHECK(((unsigned)dops.data[2] << 8 | dops.data[3]) == 312);   /* PreSkip big-endian */
    TASK_CHECK(be32(dops.data + 4) == 44100);  /* InputSampleRate big-endian */
    TASK_CHECK((short)(dops.data[8] << 8 | dops.data[9]) == (short)0xff01); /* OutputGain */
    TASK_CHECK(dops.data[10] == 255);        /* ChannelMappingFamily */
    TASK_CHECK(dops.data[11] == 4);          /* StreamCount */
    TASK_CHECK(dops.data[12] == 2);          /* CoupledCount */
    {
        static const unsigned char expect[] = { 5, 0, 255, 3, 1, 4, 5 };
        TASK_CHECK(!memcmp(dops.data + 13, expect, sizeof(expect)));
    }
    mem_file_reset(&f);
    return 0;
}

/* sample entry channelcount is streams+coupled (6), not 7. */
static int case_container_sample_entry(void)
{
    mem_file_t f = {0};
    span_t root, stsd, entry;
    static const uint32_t path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                     BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                     BOX4('s','t','b','l'), BOX4('s','t','s','d') };
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_path(root, path, ARRAY_LEN(path), &stsd));
    {
        span_t entries = { stsd.data + 8, stsd.size - 8 };
        TASK_CHECK(find_box(entries, BOX4('O','p','u','s'), &entry));
    }
    TASK_CHECK(entry.size >= 28);
    TASK_CHECK((entry.data[16] << 8 | entry.data[17]) == 6);     /* channelcount */
    TASK_CHECK(be32(entry.data + 24) >> 16 == 48000);            /* samplerate */
    /* No mp4a / esds anywhere in this file. */
    {
        long i;
        int mp4a = 0, esds = 0;
        for (i = 0; i + 4 <= f.size; i++)
        {
            if (!memcmp(f.data + i, "mp4a", 4)) mp4a++;
            if (!memcmp(f.data + i, "esds", 4)) esds++;
        }
        TASK_CHECK(!mp4a && !esds);
    }
    mem_file_reset(&f);
    return 0;
}

/* ftyp advertises iso2 for Opus files. */
static int case_container_ftyp_iso2(void)
{
    mem_file_t f = {0};
    span_t root, ftyp;
    long i;
    int found = 0;
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_box(root, BOX4('f','t','y','p'), &ftyp));
    TASK_CHECK(ftyp.size >= 8);
    for (i = 8; i + 4 <= ftyp.size; i += 4)
        if (be32(ftyp.data + i) == BOX4('i','s','o','2'))
            found = 1;
    TASK_CHECK(found);
    mem_file_reset(&f);
    return 0;
}

/* Movie timescale is 48000 and mvhd duration is the displayed length. */
static int case_container_mvhd(void)
{
    mem_file_t f = {0};
    span_t root, mvhd;
    unsigned raw = 0, i;
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_box(root, BOX4('m','o','o','v'), &root));
    TASK_CHECK(find_box(root, BOX4('m','v','h','d'), &mvhd));
    TASK_CHECK(be32(mvhd.data) >> 24 == 0);
    TASK_CHECK(be32(mvhd.data + 12) == 48000);   /* timescale */
    for (i = 0; i < ARRAY_LEN(container_durations); i++)
        raw += container_durations[i];
    TASK_CHECK(be32(mvhd.data + 16) == raw - 312);   /* duration */
    mem_file_reset(&f);
    return 0;
}

/* elst: one entry, media_time = pre-skip, segment_duration = displayed. */
static int case_container_elst(void)
{
    mem_file_t f = {0};
    span_t root, edts, elst;
    unsigned raw = 0, i;
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_box(root, BOX4('m','o','o','v'), &root));
    TASK_CHECK(find_box(root, BOX4('t','r','a','k'), &root));
    TASK_CHECK(find_box(root, BOX4('e','d','t','s'), &edts));
    TASK_CHECK(find_box(edts, BOX4('e','l','s','t'), &elst));
    TASK_CHECK(elst.size == 20);
    TASK_CHECK(be32(elst.data) == 0);              /* version 0 */
    TASK_CHECK(be32(elst.data + 4) == 1);          /* entry_count */
    for (i = 0; i < ARRAY_LEN(container_durations); i++)
        raw += container_durations[i];
    TASK_CHECK(be32(elst.data + 8) == raw - 312);  /* segment_duration */
    TASK_CHECK((int32_t)be32(elst.data + 12) == 312); /* media_time */
    TASK_CHECK(be32(elst.data + 16) >> 16 == 1);   /* rate 1.0 */
    mem_file_reset(&f);
    return 0;
}

/* stts keeps the full packet durations; no stss for Opus. */
static int case_container_stts_no_stss(void)
{
    mem_file_t f = {0};
    span_t root, trak, stbl, stts;
    static const uint32_t path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                     BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                     BOX4('s','t','b','l') };
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_path(root, path, ARRAY_LEN(path), &stbl));
    TASK_CHECK(find_box(stbl, BOX4('s','t','t','s'), &stts));
    {
        /* Runs: 120, 1920 (960+960? no: 960 then 2880 then 960,960), so
           entries are 120, 960, 2880, 960x2. */
        TASK_CHECK(be32(stts.data + 4) == 4);
        TASK_CHECK(be32(stts.data + 8) == 1 && be32(stts.data + 12) == 120);
        TASK_CHECK(be32(stts.data + 16) == 1 && be32(stts.data + 20) == 960);
        TASK_CHECK(be32(stts.data + 24) == 1 && be32(stts.data + 28) == 2880);
        TASK_CHECK(be32(stts.data + 32) == 2 && be32(stts.data + 36) == 960);
    }
    (void)trak;
    TASK_CHECK(!find_box(stbl, BOX4('s','t','s','s'), &root));
    mem_file_reset(&f);
    return 0;
}

/* sgpd/sbgp roll grouping matches the "cover 3840 frames" rule. */
static int case_container_roll(void)
{
    mem_file_t f = {0};
    span_t root, stbl, sgpd, sbgp;
    static const uint32_t path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                     BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                     BOX4('s','t','b','l') };
    TASK_CHECK(build_container_file(&f, 0));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_path(root, path, ARRAY_LEN(path), &stbl));
    TASK_CHECK(find_box(stbl, BOX4('s','g','p','d'), &sgpd));
    TASK_CHECK(find_box(stbl, BOX4('s','b','g','p'), &sbgp));

    /* sgpd version 1, grouping 'roll', default_length 2. */
    TASK_CHECK(be32(sgpd.data) >> 24 == 1);
    TASK_CHECK(be32(sgpd.data + 4) == BOX4('r','o','l','l'));
    TASK_CHECK(be32(sgpd.data + 8) == 2);
    /* sbgp version 0, grouping 'roll', every sample mapped. */
    TASK_CHECK(be32(sbgp.data) >> 24 == 0);
    TASK_CHECK(be32(sbgp.data + 4) == BOX4('r','o','l','l'));
    {
        unsigned entries = be32(sbgp.data + 8);
        unsigned mapped = 0, k, desc_count = be32(sgpd.data + 12);
        /* Distances for samples 0..4, durations 120,960,2880,960,960:
           s0 -> -(0+1) = -1   (no preceding packets)
           s1 -> -(1+1) = -2   (120 < 3840)
           s2 -> -(2+1) = -3   (120+960 < 3840)
           s3 -> -2            (2880+960 = 3840, k=2)
           s4 -> -2            (960+2880 = 3840, k=2)
           Descriptions appear in first-use order: -1, -2, -3. */
        static const unsigned short expect_dist[] = { (unsigned short)-1,
            (unsigned short)-2, (unsigned short)-3 };
        TASK_CHECK(desc_count == 3);
        for (k = 0; k < desc_count; k++)
        {
            unsigned short d = (unsigned short)(sgpd.data[16 + 2*k] << 8 |
                                                sgpd.data[17 + 2*k]);
            TASK_CHECK(d == expect_dist[k]);
        }
        /* Runs: 1 x d1, 1 x d2, 2 x d3, 1 x d2 = 4 runs */
        TASK_CHECK(entries == 4);
        for (k = 0; k < entries; k++)
            mapped += be32(sbgp.data + 12 + 8*k);
        TASK_CHECK(mapped == 5);
    }
    mem_file_reset(&f);
    return 0;
}

/* Sequential mode emits the same container semantics. */
static int case_container_sequential(void)
{
    mem_file_t f = {0};
    span_t root, ftyp, stbl, sgpd;
    static const uint32_t path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                     BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                     BOX4('s','t','b','l') };
    TASK_CHECK(build_container_file(&f, 1));
    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_box(root, BOX4('f','t','y','p'), &ftyp));
    TASK_CHECK(find_path(root, path, ARRAY_LEN(path), &stbl));
    TASK_CHECK(find_box(stbl, BOX4('s','g','p','d'), &sgpd));
    mem_file_reset(&f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Demux group                                                         */
/* ------------------------------------------------------------------ */

/* Round trip: what we write is what MP4D reads back. */
static int case_demux_roundtrip(void)
{
    mem_file_t f = {0};
    MP4D_demux_t mp4;
    unsigned raw = 0, i;
    TASK_CHECK(build_container_file(&f, 0));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track_count == 1);
    TASK_CHECK(mp4.track[0].sample_entry == 0x4f707573);
    TASK_CHECK(mp4.track[0].handler_type == MP4D_HANDLER_TYPE_SOUN);
    TASK_CHECK(mp4.track[0].opus_trim_start == 312);
    TASK_CHECK(mp4.track[0].opus_trim_end == 0);
    TASK_CHECK(mp4.track[0].dsi_bytes == 28);
    TASK_CHECK(!memcmp(mp4.track[0].dsi, "OpusHead", 8));
    TASK_CHECK(mp4.track[0].dsi[9] == 7);
    TASK_CHECK((mp4.track[0].dsi[10] | (mp4.track[0].dsi[11] << 8)) == 312);
    TASK_CHECK((mp4.track[0].dsi[18]) == 255);
    for (i = 0; i < ARRAY_LEN(container_durations); i++)
        raw += container_durations[i];
    {
        unsigned ts = 0;
        TASK_CHECK(mp4.track[0].sample_count == ARRAY_LEN(container_durations));
        for (i = 0; i < mp4.track[0].sample_count; i++)
        {
            unsigned bytes = 0, timestamp = 0, duration = 0;
            MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, i, &bytes,
                                                       &timestamp, &duration);
            TASK_CHECK(off != 0);
            TASK_CHECK(bytes > 0);
            TASK_CHECK(timestamp == ts);
            TASK_CHECK(duration == container_durations[i]);
            ts += duration;
        }
        TASK_CHECK(ts == raw);
    }
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}

/* Trims and per-packet schedule survive through sequential mode too. */
static int case_demux_sequential_roundtrip(void)
{
    mem_file_t f = {0};
    MP4D_demux_t mp4;
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(build_container_file(&f, 1));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track_count == 1);
    TASK_CHECK(mp4.track[0].sample_count == 5);
    TASK_CHECK(mp4.track[0].opus_trim_start == 312);
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}

/* A file with a non-trivial end trim reports it back. */
static int case_demux_tail_trim(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0x10,0, 0,0,0,0, 0,0, 0
    };
    unsigned char pkt[64];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    int n, track;

    n = build_multi_sample(pkt, 1, 18 /*10 ms*/, 2, 1, 0x10);   /* 480 frames */
    TASK_CHECK(n > 0);
    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 100) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track[0].opus_trim_start == 16);
    TASK_CHECK(mp4.track[0].opus_trim_end == 100);
    TASK_CHECK(mp4.track[0].sample_count == 1);
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}


/* ------------------------------------------------------------------ */
/* Hand-built MP4 material for the reader                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char data[65536];
    long size;
} blob_t;

static void blob_box(blob_t *b, uint32_t name, long at, long total_size)
{
    /* total_size includes the 8-byte box header */
    b->data[at + 0] = (unsigned char)(total_size >> 24);
    b->data[at + 1] = (unsigned char)(total_size >> 16);
    b->data[at + 2] = (unsigned char)(total_size >> 8);
    b->data[at + 3] = (unsigned char)total_size;
    b->data[at + 4] = (unsigned char)(name >> 24);
    b->data[at + 5] = (unsigned char)(name >> 16);
    b->data[at + 6] = (unsigned char)(name >> 8);
    b->data[at + 7] = (unsigned char)name;
}

static void blob_u32(blob_t *b, long at, uint32_t v)
{
    b->data[at + 0] = (unsigned char)(v >> 24);
    b->data[at + 1] = (unsigned char)(v >> 16);
    b->data[at + 2] = (unsigned char)(v >> 8);
    b->data[at + 3] = (unsigned char)v;
}

static void blob_u16(blob_t *b, long at, unsigned v)
{
    b->data[at + 0] = (unsigned char)(v >> 8);
    b->data[at + 1] = (unsigned char)v;
}

/*
 * Build a minimal, independently assembled Opus MP4. Box order is
 * deliberately non-canonical (elst precedes mdhd/stts, and an unknown
 * 'skip' box sits between edts and mdia), so the reader cannot rely on
 * what our writer happens to emit.
 *
 * durations[]/n are per-packet full durations in 48 kHz frames. The
 * displayed duration is raw - media_time - end_trim. Pass
 * media_time_override <0 to use pre_skip. drop_dops /
 * duplicate_dops exercise missing and repeated configuration records.
 */
static int build_handmade(blob_t *b, const unsigned *durations, unsigned n,
                          unsigned pre_skip, unsigned end_trim,
                          int64_t media_time_override, int drop_dops,
                          int duplicate_dops, unsigned movie_ts,
                          unsigned media_ts)
{
    long moov_at, trak_at, edts_at, elst_at, mdia_at, mdhd_at, minf_at;
    long stbl_at, stsd_at, entry_at, dops_at, stts_at, stsc_at, stsz_at, stco_at;
    long skip_at, mdat_at, at;
    unsigned raw = 0, displayed, i;
    int64_t media_time;

    memset(b, 0, sizeof(*b));
    for (i = 0; i < n; i++)
        raw += durations[i];
    media_time = media_time_override >= 0 ? media_time_override : (int64_t)pre_skip;
    displayed = raw - (unsigned)media_time - end_trim;

    /* ftyp: major mp42, compatible iso2; total 24 bytes = 8 header + 16 */
    blob_box(b, BOX4('f','t','y','p'), 0, 24);
    b->data[8] = 'm';  b->data[9] = 'p';  b->data[10] = '4'; b->data[11] = '2';
    b->data[16] = 'm'; b->data[17] = 'p'; b->data[18] = '4'; b->data[19] = '2';
    b->data[20] = 'i'; b->data[21] = 's'; b->data[22] = 'o'; b->data[23] = '2';
    b->size = 24;

    /* mdat: one 4-byte sample per packet */
    mdat_at = b->size;
    blob_box(b, BOX4('m','d','a','t'), mdat_at, 8 + 4*n);
    for (i = 0; i < n; i++)
    {
        b->data[mdat_at + 8 + 4*i + 0] = (unsigned char)(0xA0 + i);
        b->data[mdat_at + 8 + 4*i + 1] = 0xAA;
        b->data[mdat_at + 8 + 4*i + 2] = 0xBB;
        b->data[mdat_at + 8 + 4*i + 3] = 0xCC;
    }
    b->size = mdat_at + 8 + 4*n;

    moov_at = b->size;
    b->size += 8;

    /* mvhd v0: version/flags(4) + times(8) + timescale/duration(8) + rest */
    at = b->size;
    blob_box(b, BOX4('m','v','h','d'), at, 108);
    /* +8 version/flags, +12 creation, +16 modification, +20 timescale,
       +24 duration */
    blob_u32(b, at + 20, movie_ts);
    blob_u32(b, at + 24, displayed);
    b->size = at + 108;

    trak_at = b->size;
    b->size += 8;

    /* tkhd v0, 92 total */
    at = b->size;
    blob_box(b, BOX4('t','k','h','d'), at, 92);
    /* +8 version/flags, +12 creation, +16 modification, +20 track_ID,
       +24 reserved, +28 duration */
    blob_u32(b, at + 20, 1);
    blob_u32(b, at + 28, displayed);
    b->size = at + 92;

    /* edts / elst, deliberately before mdia */
    edts_at = b->size;
    b->size += 8;
    elst_at = b->size;
    blob_box(b, BOX4('e','l','s','t'), elst_at, 28);
    blob_u32(b, elst_at + 8, 0);
    blob_u32(b, elst_at + 12, 1);
    blob_u32(b, elst_at + 16, displayed);
    blob_u32(b, elst_at + 20, (uint32_t)media_time);
    blob_u16(b, elst_at + 24, 1);
    blob_u16(b, elst_at + 26, 0);
    b->size = elst_at + 28;
    blob_box(b, BOX4('e','d','t','s'), edts_at, b->size - edts_at);

    /* Unknown box the reader must skip. */
    skip_at = b->size;
    blob_box(b, BOX4('s','k','i','p'), skip_at, 12);
    memset(b->data + skip_at + 8, 0x5A, 4);
    b->size = skip_at + 12;

    mdia_at = b->size;
    b->size += 8;
    mdhd_at = b->size;
    blob_box(b, BOX4('m','d','h','d'), mdhd_at, 32);
    /* +8 version/flags, +12 creation, +16 modification, +20 timescale,
       +24 duration */
    blob_u32(b, mdhd_at + 20, media_ts);
    blob_u32(b, mdhd_at + 24, raw);
    b->size = mdhd_at + 32;

    /* hdlr 'soun' */
    at = b->size;
    blob_box(b, BOX4('h','d','l','r'), at, 32);
    blob_u32(b, at + 16, MP4D_HANDLER_TYPE_SOUN);
    b->size = at + 32;

    minf_at = b->size;
    b->size += 8;
    stbl_at = b->size;
    b->size += 8;

    stsd_at = b->size;
    b->size += 8;
    blob_u32(b, stsd_at + 8, 0);            /* version/flags */
    blob_u32(b, stsd_at + 12, 1);           /* entry_count */
    entry_at = stsd_at + 16;
    blob_box(b, BOX4('O','p','u','s'), entry_at, 36);
    memset(b->data + entry_at + 8, 0, 6);
    blob_u16(b, entry_at + 14, 1);          /* data_reference_index */
    blob_u16(b, entry_at + 24, 2);          /* encoded channel count */
    blob_u16(b, entry_at + 26, 16);         /* sample size */
    blob_u32(b, entry_at + 32, 48000u << 16);
    b->size = entry_at + 36;

    dops_at = b->size;
    {
        /* family 0, 2 output channels: 11-byte record, no mapping table */
        static const unsigned char payload[] = {
            0, 2, 0x10, 0x00, 0x44, 0xac, 0x00, 0x00, 0x00, 0x00, 0
        };
        if (drop_dops)
        {
            /* No configuration record at all. */
            b->size = dops_at;
        } else
        {
            blob_box(b, BOX4('d','O','p','s'), dops_at,
                     (long)(8 + sizeof(payload)));
            memcpy(b->data + dops_at + 8, payload, sizeof(payload));
            b->size = dops_at + 8 + (long)sizeof(payload);
            if (duplicate_dops)
            {
                long at2 = b->size;
                blob_box(b, BOX4('d','O','p','s'), at2,
                         (long)(8 + sizeof(payload)));
                memcpy(b->data + at2 + 8, payload, sizeof(payload));
                b->size = at2 + 8 + (long)sizeof(payload);
            }
        }
    }
    blob_box(b, BOX4('O','p','u','s'), entry_at, b->size - entry_at);
    blob_box(b, BOX4('s','t','s','d'), stsd_at, b->size - stsd_at);

    /* stts */
    stts_at = b->size;
    {
        unsigned runs = 1, k, start;
        for (i = 1; i < n; i++)
            if (durations[i] != durations[i - 1])
                runs++;
        blob_box(b, BOX4('s','t','t','s'), stts_at, (long)(16 + 8*runs));
        blob_u32(b, stts_at + 8, 0);
        blob_u32(b, stts_at + 12, runs);
        at = stts_at + 16;
        for (start = 0, k = 0; start < n; k++)
        {
            unsigned stop = start;
            while (stop < n && durations[stop] == durations[start])
                stop++;
            blob_u32(b, at + 8*k, stop - start);
            blob_u32(b, at + 8*k + 4, durations[start]);
            start = stop;
        }
        b->size = stts_at + 16 + 8*runs;
    }

    /* stsc: one sample per chunk */
    stsc_at = b->size;
    blob_box(b, BOX4('s','t','s','c'), stsc_at, 28);
    blob_u32(b, stsc_at + 8, 0);
    blob_u32(b, stsc_at + 12, 1);
    blob_u32(b, stsc_at + 16, 1);
    blob_u32(b, stsc_at + 20, 1);
    blob_u32(b, stsc_at + 24, 1);
    b->size = stsc_at + 28;

    /* stsz: every sample is 4 bytes */
    stsz_at = b->size;
    blob_box(b, BOX4('s','t','s','z'), stsz_at, (long)(20 + 4*n));
    blob_u32(b, stsz_at + 8, 0);
    blob_u32(b, stsz_at + 12, 4);
    blob_u32(b, stsz_at + 16, n);
    for (i = 0; i < n; i++)
        blob_u32(b, stsz_at + 20 + 4*i, 4);
    b->size = stsz_at + 20 + 4*n;

    /* stco: one chunk per sample */
    stco_at = b->size;
    blob_box(b, BOX4('s','t','c','o'), stco_at, (long)(16 + 4*n));
    blob_u32(b, stco_at + 8, 0);
    blob_u32(b, stco_at + 12, n);
    for (i = 0; i < n; i++)
        blob_u32(b, stco_at + 16 + 4*i, (uint32_t)(mdat_at + 8 + 4*i));
    b->size = stco_at + 16 + 4*n;

    blob_box(b, BOX4('s','t','b','l'), stbl_at, b->size - stbl_at);
    blob_box(b, BOX4('m','i','n','f'), minf_at, b->size - minf_at);
    blob_box(b, BOX4('m','d','i','a'), mdia_at, b->size - mdia_at);
    blob_box(b, BOX4('t','r','a','k'), trak_at, b->size - trak_at);
    blob_box(b, BOX4('m','o','o','v'), moov_at, b->size - moov_at);
    return b->size > 0;
}

static int blob_read(int64_t offset, void *buffer, size_t size, void *token)
{
    const blob_t *b = (const blob_t *)token;
    if (offset < 0 || offset >= b->size)
        return 1;
    if (offset + (long)size > b->size)
    {
        size_t avail = (size_t)(b->size - offset);
        memcpy(buffer, b->data + offset, avail);
        memset((unsigned char *)buffer + avail, 0, size - avail);
        return size != avail;
    }
    memcpy(buffer, b->data + offset, size);
    return 0;
}

/* Hand-built file with reordered boxes reads back correctly, and the
   per-packet schedule is reported verbatim (no trim folded into stts). */
static int case_demux_handmade(void)
{
    static const unsigned durations[] = { 120, 960, 2880, 960, 960 };
    blob_t b;
    MP4D_demux_t mp4;
    unsigned raw = 0, i, ts = 0;
    TASK_CHECK(build_handmade(&b, durations, ARRAY_LEN(durations),
                              312, 0, -1, 0, 0, 48000, 48000));
    for (i = 0; i < ARRAY_LEN(durations); i++)
        raw += durations[i];
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size));
    TASK_CHECK(mp4.track_count == 1);
    TASK_CHECK(mp4.timescale == 48000);
    TASK_CHECK(mp4.track[0].sample_entry == 0x4f707573);
    TASK_CHECK(mp4.track[0].opus_trim_start == 312);
    TASK_CHECK(mp4.track[0].opus_trim_end == 0);
    TASK_CHECK(mp4.track[0].sample_count == ARRAY_LEN(durations));
    TASK_CHECK(mp4.track[0].dsi_bytes == 19);
    TASK_CHECK(!memcmp(mp4.track[0].dsi, "OpusHead", 8));
    TASK_CHECK(mp4.track[0].dsi[9] == 2);
    for (i = 0; i < ARRAY_LEN(durations); i++)
    {
        unsigned bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, i, &bytes,
                                                   &timestamp, &duration);
        TASK_CHECK(bytes == 4);
        TASK_CHECK(timestamp == ts);
        TASK_CHECK(duration == durations[i]);
        TASK_CHECK(off == 32 + 4*i);   /* ftyp(24) + mdat header(8) */
        ts += duration;
    }
    TASK_CHECK(ts == raw);
    MP4D_close(&mp4);
    return 0;
}

/* First-packet hop: media_time skips a whole packet, not just pre-skip. */
static int case_demux_cross_packet_start(void)
{
    static const unsigned durations[] = { 960, 960, 960, 960 };
    blob_t b;
    MP4D_demux_t mp4;
    unsigned raw = 0, i;
    TASK_CHECK(build_handmade(&b, durations, ARRAY_LEN(durations),
                              0, 0, 1920 /* skip two packets */, 0, 0, 48000, 48000));
    for (i = 0; i < ARRAY_LEN(durations); i++)
        raw += durations[i];
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size));
    TASK_CHECK(mp4.track[0].sample_count == 4);
    TASK_CHECK(mp4.track[0].opus_trim_start == 1920);
    TASK_CHECK(mp4.track[0].opus_trim_end == 0);
    /* The full schedule is still reported; trimming is not folded in. */
    for (i = 0; i < ARRAY_LEN(durations); i++)
    {
        unsigned bytes = 0, timestamp = 0, duration = 0;
        MP4D_frame_offset(&mp4, 0, i, &bytes, &timestamp, &duration);
        TASK_CHECK(timestamp == i*960);
        TASK_CHECK(duration == 960);
    }
    TASK_CHECK(raw == 3840);
    MP4D_close(&mp4);
    return 0;
}

/* End trim after the last packet is reported as opus_trim_end. */
static int case_demux_handmade_tail(void)
{
    static const unsigned durations[] = { 960, 2880 };   /* 20 ms, 60 ms */
    blob_t b;
    MP4D_demux_t mp4;
    TASK_CHECK(build_handmade(&b, durations, ARRAY_LEN(durations),
                              300, 500, -1, 0, 0, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size));
    TASK_CHECK(mp4.track[0].opus_trim_start == 300);
    TASK_CHECK(mp4.track[0].opus_trim_end == 500);
    TASK_CHECK(mp4.track[0].sample_count == 2);
    MP4D_close(&mp4);
    return 0;
}

/* Missing dOps makes open fail and still releases everything. */
static int case_demux_missing_dops(void)
{
    static const unsigned durations[] = { 960 };
    blob_t b;
    MP4D_demux_t mp4;
    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 1, 0, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    TASK_CHECK(mp4.track == NULL || mp4.track_count == 0);
    return 0;
}

/* Duplicate dOps is rejected as well. */
static int case_demux_duplicate_dops(void)
{
    static const unsigned durations[] = { 960 };
    blob_t b;
    MP4D_demux_t mp4;
    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 0, 1, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    return 0;
}

/* Truncated material (dOps cut short) fails to open. */
static int case_demux_truncated(void)
{
    static const unsigned durations[] = { 960 };
    blob_t b;
    MP4D_demux_t mp4;
    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 0, 0, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    /* Cut inside the dOps payload so the configuration record itself is
       truncated. A merely truncated sample table tail still yields a
       usable (if empty-schedule) track for this reader; a half-written
       dOps must not. */
    {
        long cut = -1, i;
        for (i = 0; i + 4 <= b.size; i++)
            if (!memcmp(b.data + i, "dOps", 4))
                cut = i + 4 + 4;   /* box header + 4 payload bytes */
        TASK_CHECK(cut > 0);
        b.size = cut;              /* genuinely truncated material */
        TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    }
    return 0;
}

/* Non-48 kHz movie/media timescale is outside the supported subset. */
static int case_demux_bad_timescale(void)
{
    static const unsigned durations[] = { 960 };
    blob_t b;
    MP4D_demux_t mp4;
    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 0, 0, 1000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);

    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 0, 0, 48000, 1000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    return 0;
}

/* A file whose Opus track has no edit list cannot express the trim and is
   rejected (the supported subset requires the edit). */
static int case_demux_missing_elst(void)
{
    static const unsigned durations[] = { 960 };
    blob_t b;
    MP4D_demux_t mp4;
    long i;
    TASK_CHECK(build_handmade(&b, durations, 1, 0, 0, -1, 0, 0, 48000, 48000));
    /* Remove the edts box (8-byte header + 28-byte elst) and patch every
       enclosing size. This is a deliberate, structurally valid file that
       simply lacks trim information. */
    {
        long edts = -1, elst = -1;
        for (i = 0; i + 4 <= b.size; i++)
        {
            if (!memcmp(b.data + i, "edts", 4)) edts = i - 4;
            if (!memcmp(b.data + i, "elst", 4)) elst = i - 4;
        }
        TASK_CHECK(edts > 0 && elst == edts + 8);
        {
            unsigned edts_size = be32(b.data + edts);
            long remove = (long)edts_size;
            memmove(b.data + edts, b.data + edts + remove,
                    (size_t)(b.size - edts - remove));
            b.size -= remove;
            /* Patch trak/moov sizes (both include edts). */
            for (i = 0; i + 4 <= b.size; i++)
            {
                /* nothing to do: the reader tolerates a parent whose size
                   overruns because it clamps to the parent's own bound */
                break;
            }
        }
    }
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    return 0;
}

/* An edit that cannot represent the trim (segment longer than media,
   negative media time, or a non-unity rate) fails to open. */
static int case_demux_bad_edit(void)
{
    static const unsigned durations[] = { 960, 960 };
    blob_t b;
    MP4D_demux_t mp4;

    /* media_time beyond the raw duration */
    TASK_CHECK(build_handmade(&b, durations, ARRAY_LEN(durations),
                              0, 0, 5000, 0, 0, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);

    /* display duration larger than the media that remains after media_time
       (the builder computes raw - media_time - end_trim, so an oversized
       end_trim leaves a segment that cannot be represented) */
    TASK_CHECK(build_handmade(&b, durations, ARRAY_LEN(durations),
                              0, 5000, -1, 0, 0, 48000, 48000));
    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, blob_read, &b, b.size) == 0);
    return 0;
}

/* Old AAC path still works: dsi and frame queries unchanged. */
static int case_demux_aac_unchanged(void)
{
    mem_file_t f = {0};
    MP4D_demux_t mp4;
    MP4E_mux_t *mux;
    MP4E_track_t tr;
    unsigned char dsi[2] = { 0x12, 0x10 };
    unsigned char sample[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int track;

    memset(&tr, 0, sizeof(tr));
    tr.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    tr.track_media_kind = e_audio;
    tr.time_scale = 48000;
    tr.u.a.channelcount = 2;
    memcpy(tr.language, "und", 3);

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_track(mux, &tr);
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_set_dsi(mux, track, dsi, 2) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_put_sample(mux, track, sample, 8, 1024, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track_count == 1);
    TASK_CHECK(mp4.track[0].dsi_bytes == 2);
    TASK_CHECK(mp4.track[0].dsi[0] == 0x12 && mp4.track[0].dsi[1] == 0x10);
    TASK_CHECK(mp4.track[0].handler_type == MP4D_HANDLER_TYPE_SOUN);
    TASK_CHECK(mp4.track[0].SampleDescription.audio.samplerate_hz == 48000);
    TASK_CHECK(mp4.track[0].SampleDescription.audio.channelcount == 2);
    {
        unsigned bytes = 0, ts = 0, dur = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, 0, 0, &bytes, &ts, &dur);
        TASK_CHECK(off != 0 && bytes == 8 && ts == 0 && dur == 1024);
    }
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}


/* Mixed video + Opus: the movie duration is the longest displayed track,
   and the video track keeps its existing semantics. */
static int case_container_mixed_video(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0x40,0, 0,0,0,0, 0,0, 0
    };
    static const unsigned char sps[] = { 0, 2, 0x67, 0x42 };
    static const unsigned char pps[] = { 0, 1, 0x68 };
    unsigned char pkt[64];
    unsigned char nal[8];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    MP4E_track_t vt;
    int vtrack, otrack, n, i;
    unsigned opus_display = 0;

    memset(&vt, 0, sizeof(vt));
    vt.object_type_indication = MP4_OBJECT_TYPE_AVC;
    vt.track_media_kind = e_video;
    vt.time_scale = 90000;
    vt.u.v.width = 16;
    vt.u.v.height = 16;
    memcpy(vt.language, "und", 3);

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    vtrack = MP4E_add_track(mux, &vt);
    TASK_CHECK(vtrack == 0);
    TASK_CHECK(MP4E_set_sps(mux, vtrack, sps, (int)sizeof(sps)) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_set_pps(mux, vtrack, pps, (int)sizeof(pps)) == MP4E_STATUS_OK);
    otrack = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(otrack == 1);

    /* Three video frames at 30 fps: 9000 / 90000 s = 100 ms. */
    for (i = 0; i < 3; i++)
    {
        nal[0] = 0; nal[1] = 0; nal[2] = 0; nal[3] = 2;
        nal[4] = 0x65; nal[5] = 0xAA;
        TASK_CHECK(MP4E_put_sample(mux, vtrack, nal, 6, 3000,
                                   i == 0 ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT)
                   == MP4E_STATUS_OK);
    }

    /* Three 20 ms Opus packets: 2880 frames total. pre_skip is 64, so the
       displayed length is 2816 frames (58.7 ms) -- the video track is
       longer and must win the movie duration. */
    n = build_multi_sample(pkt, 1, 19, 2, 1, 0x10);
    TASK_CHECK(n > 0);
    for (i = 0; i < 3; i++)
        TASK_CHECK(MP4E_put_opus_packet(mux, otrack, pkt, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, otrack, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track_count == 2);
    TASK_CHECK(mp4.timescale == 48000);
    TASK_CHECK(mp4.track[0].handler_type == MP4D_HANDLER_TYPE_VIDE);
    TASK_CHECK(mp4.track[0].sample_count == 3);
    TASK_CHECK(mp4.track[1].sample_entry == 0x4f707573);
    for (i = 0; i < 3; i++)
    {
        unsigned b = 0, t = 0, d = 0;
        MP4D_frame_offset(&mp4, 1, i, &b, &t, &d);
        opus_display += d;
    }
    TASK_CHECK(opus_display == 2880);
    TASK_CHECK(mp4.track[1].opus_trim_start == 64);
    /* Video display (100 ms) is longer than Opus display (2816/48000 s). */
    TASK_CHECK(mp4.duration_lo == 9000 * 48000 / 90000);
    TASK_CHECK(mp4.duration_lo > opus_display - 64);
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}

/* Legal zero-byte frames, independently-switched mono/stereo TOCs, and
   the exact 120 ms limit boundary. */
static int case_framing_edges(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0,0, 0,0,0,0, 0,0, 255, 2,0, 0,1
    };
    unsigned char pkt[4096];
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    int track, n;

    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);

    /* Stream 0 (mono TOC 0x00) has a legal zero-length frame; stream 1
       (stereo TOC 0x04) carries four bytes. The TOCs differ in channel
       mode, which is allowed as long as the durations agree. */
    n = 0;
    pkt[n++] = 0x00;                 /* config 0, mono, code 0, self-delimited */
    pkt[n++] = 0x00;                 /* frame length 0 */
    pkt[n++] = 0x04;                 /* config 0, stereo, code 0, regular */
    pkt[n++] = 0xAA; pkt[n++] = 0xBB; pkt[n++] = 0xCC; pkt[n++] = 0xDD;
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);
    mem_file_reset(&f);

    /* Exactly 48 x 2.5 ms = 120 ms is legal; 49 frames would exceed the
       120 ms packet limit. */
    {
        static const unsigned char one_head[19] = {
            'O','p','u','s','H','e','a','d', 1, 1, 0,0, 0,0,0,0, 0,0, 0
        };
        unsigned char big[4096];
        mem_file_t g = {0};
        MP4E_mux_t *m2 = MP4E_open(0, 0, &g, mem_write);
        int t2;
        TASK_CHECK(m2 != NULL);
        t2 = MP4E_add_opus_track(m2, one_head, (int)sizeof(one_head));
        TASK_CHECK(t2 == 0);
        n = build_stream(big, 16 /*2.5 ms*/, 3, 0, 1, 48, 0x11, 0);
        TASK_CHECK(n > 0);
        TASK_CHECK(MP4E_put_opus_packet(m2, t2, big, n) == MP4E_STATUS_OK);

        /* Same 48-frame shape declared with a 60 ms config would be
           48 * 2880 > 5760 and must be rejected. */
        n = build_stream(big, 3 /*60 ms*/, 3, 0, 1, 4, 0x22, 0);
        TASK_CHECK(n > 0);
        TASK_CHECK(MP4E_put_opus_packet(m2, t2, big, n) != MP4E_STATUS_OK);
        TASK_CHECK(MP4E_finish_opus(m2, t2, 0) == MP4E_STATUS_OK);
        TASK_CHECK(MP4E_close(m2) == MP4E_STATUS_OK);
        mem_file_reset(&g);
    }
    return 0;
}

/* Family 0 (mono/stereo, no mapping table) uses an 11-byte dOps and a
   19-byte reconstructed OpusHead. */
static int case_container_family0(void)
{
    static const unsigned char head[] = {
        'O','p','u','s','H','e','a','d', 1, 2, 0x58,0x02 /*600*/, 0x80,0xbb,0,0,
        0x34,0x12 /*gain=0x1234*/, 0
    };
    mem_file_t f = {0};
    MP4E_mux_t *mux;
    MP4D_demux_t mp4;
    unsigned char pkt[64];
    span_t root, stsd, entry, dops;
    static const uint32_t path[] = { BOX4('m','o','o','v'), BOX4('t','r','a','k'),
                                     BOX4('m','d','i','a'), BOX4('m','i','n','f'),
                                     BOX4('s','t','b','l'), BOX4('s','t','s','d') };
    int track, n;

    n = build_multi_sample(pkt, 1, 19, 2, 1, 0x10);
    TASK_CHECK(n > 0);
    mux = MP4E_open(0, 0, &f, mem_write);
    TASK_CHECK(mux != NULL);
    track = MP4E_add_opus_track(mux, head, (int)sizeof(head));
    TASK_CHECK(track == 0);
    TASK_CHECK(MP4E_put_opus_packet(mux, track, pkt, n) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_finish_opus(mux, track, 0) == MP4E_STATUS_OK);
    TASK_CHECK(MP4E_close(mux) == MP4E_STATUS_OK);

    root.data = f.data; root.size = f.size;
    TASK_CHECK(find_path(root, path, ARRAY_LEN(path), &stsd));
    {
        span_t entries = { stsd.data + 8, stsd.size - 8 };
        TASK_CHECK(find_box(entries, BOX4('O','p','u','s'), &entry));
    }
    TASK_CHECK((entry.data[16] << 8 | entry.data[17]) == 2);   /* streams+coupled */
    {
        span_t children = { entry.data + 28, entry.size - 28 };
        TASK_CHECK(find_box(children, BOX4('d','O','p','s'), &dops));
    }
    TASK_CHECK(dops.size == 11);
    TASK_CHECK(dops.data[0] == 0);
    TASK_CHECK(dops.data[1] == 2);
    TASK_CHECK(((unsigned)dops.data[2] << 8 | dops.data[3]) == 600);
    TASK_CHECK(be32(dops.data + 4) == 48000);
    TASK_CHECK((short)(dops.data[8] << 8 | dops.data[9]) == (short)0x1234);
    TASK_CHECK(dops.data[10] == 0);

    memset(&mp4, 0, sizeof(mp4));
    TASK_CHECK(MP4D_open(&mp4, mem_read, &f, f.size));
    TASK_CHECK(mp4.track[0].dsi_bytes == 19);
    TASK_CHECK(!memcmp(mp4.track[0].dsi, "OpusHead", 8));
    TASK_CHECK(mp4.track[0].dsi[9] == 2);
    TASK_CHECK((mp4.track[0].dsi[10] | (mp4.track[0].dsi[11] << 8)) == 600);
    TASK_CHECK((mp4.track[0].dsi[16] | (mp4.track[0].dsi[17] << 8)) == 0x1234);
    TASK_CHECK(mp4.track[0].dsi[18] == 0);
    TASK_CHECK(mp4.track[0].opus_trim_start == 600);
    MP4D_close(&mp4);
    mem_file_reset(&f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define CASE(g, n) { g, #n, n }

const struct task_case task_cases[] = {
    CASE("framing",   case_framing_multi_stream),
    CASE("framing",   case_framing_bad_lengths),
    CASE("framing",   case_framing_duration_mismatch),
    CASE("framing",   case_framing_code3),
    CASE("framing",   case_framing_edges),
    CASE("framing",   case_framing_reject_keeps_sequential),
    CASE("framing",   case_framing_head_validation),
    CASE("framing",   case_framing_put_sample_rejected),
    CASE("framing",   case_framing_finish_rules),
    CASE("framing",   case_framing_unfinished_close),
    CASE("container", case_container_dops),
    CASE("container", case_container_sample_entry),
    CASE("container", case_container_ftyp_iso2),
    CASE("container", case_container_mvhd),
    CASE("container", case_container_elst),
    CASE("container", case_container_stts_no_stss),
    CASE("container", case_container_roll),
    CASE("container", case_container_sequential),
    CASE("container", case_container_mixed_video),
    CASE("container", case_container_family0),
    CASE("demux",     case_demux_roundtrip),
    CASE("demux",     case_demux_sequential_roundtrip),
    CASE("demux",     case_demux_tail_trim),
    CASE("demux",     case_demux_handmade),
    CASE("demux",     case_demux_cross_packet_start),
    CASE("demux",     case_demux_handmade_tail),
    CASE("demux",     case_demux_missing_dops),
    CASE("demux",     case_demux_duplicate_dops),
    CASE("demux",     case_demux_truncated),
    CASE("demux",     case_demux_bad_timescale),
    CASE("demux",     case_demux_missing_elst),
    CASE("demux",     case_demux_bad_edit),
    CASE("demux",     case_demux_aac_unchanged),
};

const size_t task_case_count = ARRAY_LEN(task_cases);
