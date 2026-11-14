/*
    Tests for MP4E_faststart(): moov relocation rewrite.
    Groups: layout (box layout of the rewrite), offsets (chunk offset
    remapping), stream (callback I/O behavior).
*/
#include "../minimp4.h"
#include "task_support/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/************************************************************************/
/*          helpers                                                     */
/************************************************************************/

static uint32_t t_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t t_be64(const unsigned char *p)
{
    return ((uint64_t)t_be32(p) << 32) | t_be32(p + 4);
}
static void t_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
static void t_put64(unsigned char *p, uint64_t v)
{
    t_put32(p, (uint32_t)(v >> 32));
    t_put32(p + 4, (uint32_t)v);
}
static uint32_t type4(const char *s)
{
    return ((uint32_t)(unsigned char)s[0] << 24) | ((uint32_t)(unsigned char)s[1] << 16) |
           ((uint32_t)(unsigned char)s[2] << 8) | (uint32_t)(unsigned char)s[3];
}

/* growable byte buffer for building synthetic files */
typedef struct { unsigned char *p; size_t n, cap; } buf_t;
static void buf_grow(buf_t *b, size_t extra)
{
    if (b->n + extra > b->cap)
    {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + extra) nc *= 2;
        b->p = (unsigned char *)realloc(b->p, nc);
        if (!b->p) abort();
        b->cap = nc;
    }
}
static void buf_put(buf_t *b, const void *d, size_t n)
{
    buf_grow(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void buf_u8(buf_t *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    buf_put(b, &c, 1);
}
static void buf_32(buf_t *b, uint32_t v)
{
    unsigned char t[4];
    t_put32(t, v);
    buf_put(b, t, 4);
}
static void buf_64(buf_t *b, uint64_t v)
{
    unsigned char t[8];
    t_put64(t, v);
    buf_put(b, t, 8);
}
static void box_open(buf_t *b, const char *type, size_t *mark)
{
    *mark = b->n;
    buf_32(b, 0);
    buf_put(b, type, 4);
}
static void box_close(buf_t *b, size_t mark)
{
    t_put32(b->p + mark, (uint32_t)(b->n - mark));
}
static void put_ftyp(buf_t *b)
{
    buf_32(b, 24);
    buf_put(b, "ftyp", 4);
    buf_put(b, "isom", 4);
    buf_32(b, 0);
    buf_put(b, "isom", 4);
    buf_put(b, "mp41", 4);
}
static void put_stco(buf_t *b, const uint32_t *entries, int n)
{
    size_t m;
    int i;
    box_open(b, "stco", &m);
    buf_32(b, 0);
    buf_32(b, (uint32_t)n);
    for (i = 0; i < n; i++) buf_32(b, entries[i]);
    box_close(b, m);
}
static void put_co64(buf_t *b, const uint64_t *entries, int n)
{
    size_t m;
    int i;
    box_open(b, "co64", &m);
    buf_32(b, 0);
    buf_32(b, (uint32_t)n);
    for (i = 0; i < n; i++) buf_64(b, entries[i]);
    box_close(b, m);
}
/* append trak/mdia/minf/[dinf]/stbl/[stco|co64] to a moov-body buffer */
static void put_trak(buf_t *moov, int with_dinf, const void *entries, int n, int is_co64)
{
    size_t m_trak, m_mdia, m_minf, m_stbl;
    box_open(moov, "trak", &m_trak);
    box_open(moov, "mdia", &m_mdia);
    box_open(moov, "minf", &m_minf);
    if (with_dinf)
    {
        size_t m_dinf, m_dref, m_url;
        box_open(moov, "dinf", &m_dinf);
        box_open(moov, "dref", &m_dref);
        buf_32(moov, 0);            /* version/flags */
        buf_32(moov, 1);            /* entry_count */
        box_open(moov, "url ", &m_url);
        buf_32(moov, 1);            /* flags & 1: self-contained */
        box_close(moov, m_url);
        box_close(moov, m_dref);
        box_close(moov, m_dinf);
    }
    box_open(moov, "stbl", &m_stbl);
    if (is_co64)
        put_co64(moov, (const uint64_t *)entries, n);
    else
        put_stco(moov, (const uint32_t *)entries, n);
    box_close(moov, m_stbl);
    box_close(moov, m_minf);
    box_close(moov, m_mdia);
    box_close(moov, m_trak);
}

/* instrumented in-memory input/output pair used as callback token */
typedef struct
{
    const unsigned char *src;
    uint64_t src_n;
    uint64_t r_calls, r_max;
    int r_bad;
    uint64_t r_fail_call;   /* fail at this 1-based read call, 0 = never */
    uint64_t w_calls, w_max, w_expect;
    int w_bad;
    uint64_t w_fail_call;   /* fail at this 1-based write call, 0 = never */
    uint64_t w_fail_off;    /* fail when write offset >= this */
    unsigned char *out;
    uint64_t out_n, out_cap;
} io_t;
static void io_init(io_t *io, const unsigned char *src, uint64_t n)
{
    memset(io, 0, sizeof(*io));
    io->src = src;
    io->src_n = n;
    io->w_fail_off = UINT64_MAX;
}
static void io_free(io_t *io)
{
    free(io->out);
    io->out = 0;
}
static int io_read(int64_t off, void *dst, size_t n, void *tok)
{
    io_t *io = (io_t *)tok;
    io->r_calls++;
    if (!n || n > 65536) io->r_bad = 1;
    if (n > io->r_max) io->r_max = n;
    if (io->r_fail_call && io->r_calls >= io->r_fail_call) return 1;
    if (off < 0 || (uint64_t)off > io->src_n || (uint64_t)n > io->src_n - (uint64_t)off)
    {
        io->r_bad = 1;
        return 1;
    }
    memcpy(dst, io->src + off, n);
    return 0;
}
static int io_write(int64_t off, const void *src, size_t n, void *tok)
{
    io_t *io = (io_t *)tok;
    io->w_calls++;
    if ((uint64_t)off != io->w_expect) io->w_bad = 1;
    if (!n || n > 65536) io->w_bad = 1;
    if (n > io->w_max) io->w_max = n;
    if (io->w_fail_call && io->w_calls >= io->w_fail_call) return 1;
    if ((uint64_t)off >= io->w_fail_off) return 1;
    if (io->out_n + n > io->out_cap)
    {
        uint64_t nc = io->out_cap ? io->out_cap * 2 : 1 << 16;
        unsigned char *np;
        while (nc < io->out_n + n) nc *= 2;
        np = (unsigned char *)realloc(io->out, (size_t)nc);
        if (!np) return 1;
        io->out = np;
        io->out_cap = nc;
    }
    memcpy(io->out + io->out_n, src, n);
    io->out_n += n;
    io->w_expect = (uint64_t)off + n;
    return 0;
}

/* simple blob reader for MP4D_open */
typedef struct { const unsigned char *p; uint64_t n; } blob_t;
static int blob_read(int64_t off, void *dst, size_t n, void *tok)
{
    blob_t *b = (blob_t *)tok;
    if (off < 0 || (uint64_t)off > b->n || (uint64_t)n > b->n - (uint64_t)off) return 1;
    memcpy(dst, b->p + off, n);
    return 0;
}

/* independent top-level box walker */
typedef struct { uint32_t type; uint64_t off, size, hdr; } box_t;
static int walk_boxes(const unsigned char *p, uint64_t n, box_t *boxes, int max)
{
    int count = 0;
    uint64_t pos = 0;
    while (pos < n)
    {
        uint64_t sz, hdr = 8;
        if (n - pos < 8) return -1;
        sz = t_be32(p + pos);
        if (sz == 1)
        {
            if (n - pos < 16) return -1;
            sz = t_be64(p + pos + 8);
            hdr = 16;
        }
        if (sz < hdr || sz > n - pos) return -1;
        if (count < max)
        {
            boxes[count].type = t_be32(p + pos + 4);
            boxes[count].off = pos;
            boxes[count].size = sz;
            boxes[count].hdr = hdr;
        }
        count++;
        pos += sz;
    }
    return count;
}

/* locate stco/co64 tables inside a moov box, descending trak/mdia/minf/stbl */
typedef struct { uint32_t type; uint64_t coff, csize; } tab_t;
typedef struct { tab_t *tabs; int max, count; } tab_scan_t;
static int scan_tables(const unsigned char *buf, uint64_t off, uint64_t n, tab_scan_t *sc, int depth)
{
    uint64_t pos = 0;
    if (depth > 32) return -1;
    while (pos < n)
    {
        uint64_t sz, hdr = 8;
        uint32_t type;
        if (n - pos < 8) return -1;
        sz = t_be32(buf + off + pos);
        type = t_be32(buf + off + pos + 4);
        if (sz == 1)
        {
            if (n - pos < 16) return -1;
            sz = t_be64(buf + off + pos + 8);
            hdr = 16;
        }
        if (sz < hdr || sz > n - pos) return -1;
        if (type == type4("trak") || type == type4("mdia") || type == type4("minf") || type == type4("stbl"))
        {
            if (scan_tables(buf, off + pos + hdr, sz - hdr, sc, depth + 1)) return -1;
        } else if (type == type4("stco") || type == type4("co64"))
        {
            if (sc->count < sc->max)
            {
                sc->tabs[sc->count].type = type;
                sc->tabs[sc->count].coff = off + pos + hdr;
                sc->tabs[sc->count].csize = sz - hdr;
            }
            sc->count++;
        }
        pos += sz;
    }
    return 0;
}

static unsigned char *slurp(const char *path, uint64_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long sz;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return 0; }
    rewind(f);
    p = (unsigned char *)malloc((size_t)sz);
    if (!p) { fclose(f); return 0; }
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return 0; }
    fclose(f);
    *n = (uint64_t)sz;
    return p;
}

/************************************************************************/
/*          layout group                                                */
/************************************************************************/

/* ftyp, free, mdat, skip, moov, unknown -> ftyp, moov, mdat, unknown */
static int case_layout_order_free_skip(void)
{
    buf_t in = { 0 }, moov = { 0 };
    io_t io;
    size_t m;
    uint64_t mdat_payload, moov_size, new_payload;
    uint32_t entries[2];
    box_t boxes[8];
    tab_t tabs[4];
    tab_scan_t sc;
    int rc, n, i;

    put_ftyp(&in);
    buf_32(&in, 12); buf_put(&in, "free", 4); buf_32(&in, 0xdeadbeef);
    mdat_payload = in.n + 8;
    buf_32(&in, 108); buf_put(&in, "mdat", 4);
    for (i = 0; i < 100; i++) buf_u8(&in, (unsigned)(i * 7 + 3) & 0xff);
    buf_32(&in, 8); buf_put(&in, "skip", 4);
    entries[0] = (uint32_t)mdat_payload;
    entries[1] = (uint32_t)(mdat_payload + 50);
    box_open(&moov, "moov", &m);
    put_trak(&moov, 0, entries, 2, 0);
    box_close(&moov, m);
    moov_size = moov.n;
    buf_put(&in, moov.p, moov.n);
    buf_32(&in, 28); buf_put(&in, "abcd", 4);
    for (i = 0; i < 20; i++) buf_u8(&in, 0xa0 + i);

    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    TASK_CHECK(!io.r_bad && !io.w_bad);

    n = walk_boxes(io.out, io.out_n, boxes, 8);
    TASK_CHECK(n == 4);
    TASK_CHECK(boxes[0].type == type4("ftyp"));
    TASK_CHECK(boxes[1].type == type4("moov"));
    TASK_CHECK(boxes[2].type == type4("mdat"));
    TASK_CHECK(boxes[3].type == type4("abcd"));
    TASK_CHECK(boxes[1].size == moov_size);
    TASK_CHECK(!memcmp(io.out, in.p, 24));                      /* ftyp preserved */
    new_payload = boxes[2].off + 8;
    TASK_CHECK(boxes[2].size == 108);
    for (i = 0; i < 100; i++)                                   /* mdat payload preserved */
        TASK_CHECK(io.out[new_payload + i] == (unsigned char)((i * 7 + 3) & 0xff));
    for (i = 0; i < 20; i++)                                    /* unknown box preserved */
        TASK_CHECK(io.out[boxes[3].off + 8 + i] == (unsigned char)(0xa0 + i));

    memset(&sc, 0, sizeof(sc));
    sc.tabs = tabs; sc.max = 4;
    TASK_CHECK(scan_tables(io.out, boxes[1].off + 8, boxes[1].size - 8, &sc, 0) == 0);
    TASK_CHECK(sc.count == 1);
    TASK_CHECK(tabs[0].type == type4("stco"));
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 4) == 2);
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 8) == new_payload);
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 12) == new_payload + 50);

    free(in.p); free(moov.p); io_free(&io);
    return 0;
}

/* 64-bit extended headers on mdat and moov must be handled and preserved */
static int case_layout_extended_headers(void)
{
    buf_t in = { 0 }, body = { 0 };
    io_t io;
    uint64_t mdat_payload, new_payload;
    uint32_t entry;
    box_t boxes[4];
    tab_t tabs[4];
    tab_scan_t sc;
    int rc, n, i;

    put_ftyp(&in);
    mdat_payload = in.n + 16;   /* extended header */
    buf_32(&in, 1); buf_put(&in, "mdat", 4); buf_64(&in, 16 + 40);
    for (i = 0; i < 40; i++) buf_u8(&in, 0x40 + i);
    entry = (uint32_t)(mdat_payload + 4);
    put_trak(&body, 0, &entry, 1, 0);
    buf_32(&in, 1); buf_put(&in, "moov", 4); buf_64(&in, 16 + body.n);
    buf_put(&in, body.p, body.n);

    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);

    n = walk_boxes(io.out, io.out_n, boxes, 4);
    TASK_CHECK(n == 3);
    TASK_CHECK(boxes[0].type == type4("ftyp"));
    TASK_CHECK(boxes[1].type == type4("moov"));
    TASK_CHECK(boxes[2].type == type4("mdat"));
    TASK_CHECK(boxes[1].hdr == 16);                             /* moov keeps extended header */
    TASK_CHECK(t_be32(io.out + boxes[1].off) == 1);
    TASK_CHECK(boxes[2].hdr == 16);                             /* mdat header preserved */
    TASK_CHECK(!memcmp(io.out + boxes[2].off, in.p + 24, 16));
    new_payload = boxes[2].off + 16;
    for (i = 0; i < 40; i++)
        TASK_CHECK(io.out[new_payload + i] == (unsigned char)(0x40 + i));

    memset(&sc, 0, sizeof(sc));
    sc.tabs = tabs; sc.max = 4;
    TASK_CHECK(scan_tables(io.out, boxes[1].off + 16, boxes[1].size - 16, &sc, 0) == 0);
    TASK_CHECK(sc.count == 1);
    TASK_CHECK(tabs[0].type == type4("stco"));
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 8) == new_payload + 4);

    free(in.p); free(body.p); io_free(&io);
    return 0;
}

/* virtual >4GB input: the second stco table only overflows after the first
   table's stco->co64 upgrade grows the moov box; the write callback stops
   the rewrite once the whole new moov has been delivered */
typedef struct
{
    const unsigned char *ftyp;   /* 24 bytes at offset 0 */
    const unsigned char *mdat2;  /* 64 bytes */
    const unsigned char *mdat1;  /* 24 bytes */
    const unsigned char *moov;   /* m0 bytes */
    uint64_t m0, B;
    uint32_t B32;
    uint64_t reads, max_chunk;
    int bad;
} virt_t;
static int virt_read(int64_t off, void *dst, size_t n, void *tok)
{
    virt_t *v = (virt_t *)tok;
    unsigned char *d = (unsigned char *)dst;
    uint64_t o = (uint64_t)off, end = o + n, k;
    v->reads++;
    if (!n || n > 65536) { v->bad = 1; return 1; }
    if (n > v->max_chunk) v->max_chunk = n;
    while (o < end)
    {
        if (o < 24)
        {
            *d++ = v->ftyp[o++];
        } else if (o < 24 + v->B)
        {
            k = o - 24;
            if (k < 4) *d = (unsigned char)(v->B32 >> (8 * (3 - k)));
            else if (k < 8) *d = (unsigned char)"junk"[k - 4];
            else *d = 0;
            d++; o++;
        } else if (o < 24 + v->B + 64)
        {
            *d++ = v->mdat2[o - 24 - v->B]; o++;
        } else if (o < 24 + v->B + 88)
        {
            *d++ = v->mdat1[o - 24 - v->B - 64]; o++;
        } else if (o < 24 + v->B + 88 + v->m0)
        {
            *d++ = v->moov[o - 24 - v->B - 88]; o++;
        } else
        {
            v->bad = 1;
            return 1;
        }
    }
    return 0;
}
static size_t find_nth(const unsigned char *p, size_t n, const char *pat, int nth)
{
    size_t i;
    for (i = 0; i + 4 <= n; i++)
        if (!memcmp(p + i, pat, 4) && !nth--)
            return i;
    return (size_t)-1;
}
typedef struct { virt_t *v; io_t *io; } virt_io_t;
static int virt_read_cb(int64_t off, void *dst, size_t n, void *tok)
{
    return virt_read(off, dst, n, ((virt_io_t *)tok)->v);
}
static int virt_write_cb(int64_t off, const void *src, size_t n, void *tok)
{
    return io_write(off, src, n, ((virt_io_t *)tok)->io);
}
static int case_layout_64bit_cascade(void)
{
    buf_t ftyp = { 0 }, moov = { 0 }, mdat1 = { 0 }, mdat2 = { 0 };
    size_t m, s1, s2;
    uint32_t zero_entry = 0;
    uint64_t B, P1, P2, total, m0;
    virt_t v;
    io_t io;
    int rc, i;
    tab_t tabs[4];
    tab_scan_t sc;

    put_ftyp(&ftyp);
    box_open(&moov, "moov", &m);
    put_trak(&moov, 0, &zero_entry, 1, 0);   /* trak1: chunk in mdat1 */
    put_trak(&moov, 0, &zero_entry, 1, 0);   /* trak2: chunk in mdat2 */
    box_close(&moov, m);
    m0 = moov.n;
    TASK_CHECK(m0 == 112);

    /* input layout: ftyp(24) | junk(B) | mdat2(64) | mdat1(24) | moov(m0) */
    B = 0xFFFFFFFFull - m0 - 34;
    P2 = 24 + B + 8;              /* mdat2 payload input offset, fits uint32 */
    P1 = 24 + B + 64 + 8;         /* mdat1 payload input offset, fits uint32 */
    TASK_CHECK(P1 <= 0xFFFFFFFFu && P2 <= 0xFFFFFFFFu);
    s1 = find_nth(moov.p, moov.n, "stco", 0);
    s2 = find_nth(moov.p, moov.n, "stco", 1);
    TASK_CHECK(s1 != (size_t)-1 && s2 != (size_t)-1);
    t_put32(moov.p + s1 + 12, (uint32_t)P1);
    t_put32(moov.p + s2 + 12, (uint32_t)P2);

    buf_32(&mdat2, 64); buf_put(&mdat2, "mdat", 4);
    for (i = 0; i < 56; i++) buf_u8(&mdat2, i);
    buf_32(&mdat1, 24); buf_put(&mdat1, "mdat", 4);
    for (i = 0; i < 16; i++) buf_u8(&mdat1, 0xf0 + i);
    total = 24 + B + 64 + 24 + m0;

    memset(&v, 0, sizeof(v));
    v.ftyp = ftyp.p; v.mdat2 = mdat2.p; v.mdat1 = mdat1.p; v.moov = moov.p;
    v.m0 = m0; v.B = B; v.B32 = (uint32_t)B;

    /* both tables upgrade: moov grows by 8, from 112 to 120 */
    io_init(&io, 0, 0);
    io.w_fail_off = 24 + m0 + 8;
    {
        virt_io_t tok;
        tok.v = &v;
        tok.io = &io;
        rc = MP4E_faststart(total, &tok, virt_read_cb, virt_write_cb);
    }
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);   /* stopped by the write callback */
    TASK_CHECK(!v.bad && v.max_chunk <= 65536);
    TASK_CHECK(!io.w_bad);
    TASK_CHECK(io.out_n == 24 + m0 + 8);              /* ftyp + new moov only */
    TASK_CHECK(!memcmp(io.out, ftyp.p, 24));
    TASK_CHECK(t_be32(io.out + 24) == m0 + 8);
    TASK_CHECK(t_be32(io.out + 28) == type4("moov"));

    memset(&sc, 0, sizeof(sc));
    sc.tabs = tabs; sc.max = 4;
    TASK_CHECK(scan_tables(io.out, 24 + 8, m0, &sc, 0) == 0);
    TASK_CHECK(sc.count == 2);
    TASK_CHECK(tabs[0].type == type4("co64"));        /* upgraded from stco */
    TASK_CHECK(tabs[1].type == type4("co64"));
    TASK_CHECK(t_be64(io.out + tabs[0].coff + 8) == P1 + m0 + 8);
    TASK_CHECK(t_be64(io.out + tabs[1].coff + 8) == P2 + m0 + 8);

    free(ftyp.p); free(moov.p); free(mdat1.p); free(mdat2.p); io_free(&io);
    return 0;
}

/* inputs that must be rejected with MP4E_STATUS_BAD_ARGUMENTS before any write */
static int case_layout_bad_inputs(void)
{
    io_t io;
    buf_t in = { 0 }, moov = { 0 };
    size_t m;
    uint32_t entry;
    int rc;

    /* oversize length, NULL callbacks, zero length: no callback invoked */
    io_init(&io, 0, 0);
    TASK_CHECK(MP4E_faststart(UINT64_MAX, &io, io_read, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.r_calls == 0 && io.w_calls == 0);
    TASK_CHECK(MP4E_faststart(0, &io, io_read, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_faststart(64, &io, NULL, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_faststart(64, &io, io_read, NULL) == MP4E_STATUS_BAD_ARGUMENTS);

    /* size-0 box */
    put_ftyp(&in);
    buf_32(&in, 0); buf_put(&in, "free", 4); buf_32(&in, 1);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0 && io.out_n == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* truncated box */
    buf_32(&in, 100); buf_put(&in, "ftyp", 4); buf_32(&in, 0);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* moof box (fragmented input) */
    entry = 32;
    box_open(&moov, "moov", &m);
    put_trak(&moov, 0, &entry, 1, 0);
    box_close(&moov, m);
    put_ftyp(&in);
    buf_32(&in, 12); buf_put(&in, "mdat", 4); buf_32(&in, 0x12345678);
    buf_put(&in, moov.p, moov.n);
    buf_32(&in, 8); buf_put(&in, "moof", 4);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* two moov boxes */
    put_ftyp(&in);
    buf_put(&in, moov.p, moov.n);
    buf_put(&in, moov.p, moov.n);
    buf_32(&in, 12); buf_put(&in, "mdat", 4); buf_32(&in, 0);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* no mdat box */
    put_ftyp(&in);
    buf_put(&in, moov.p, moov.n);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* no ftyp box */
    buf_put(&in, moov.p, moov.n);
    buf_32(&in, 12); buf_put(&in, "mdat", 4); buf_32(&in, 0);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* cmov inside moov */
    put_ftyp(&in);
    buf_32(&in, 12); buf_put(&in, "mdat", 4); buf_32(&in, 0);
    buf_32(&in, 8 + 8); buf_put(&in, "moov", 4);
    buf_32(&in, 8); buf_put(&in, "cmov", 4);
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    /* external data reference (url without the self-contained flag) */
    {
        buf_t xm = { 0 };
        size_t m_trak, m_mdia, m_minf, m_dinf, m_dref, m_url, m_stbl;
        box_open(&xm, "moov", &m);
        box_open(&xm, "trak", &m_trak);
        box_open(&xm, "mdia", &m_mdia);
        box_open(&xm, "minf", &m_minf);
        box_open(&xm, "dinf", &m_dinf);
        box_open(&xm, "dref", &m_dref);
        buf_32(&xm, 0);
        buf_32(&xm, 1);
        box_open(&xm, "url ", &m_url);
        buf_32(&xm, 0);             /* flags & 1 clear: external data */
        box_close(&xm, m_url);
        box_close(&xm, m_dref);
        box_close(&xm, m_dinf);
        box_open(&xm, "stbl", &m_stbl);
        entry = 32;
        put_stco(&xm, &entry, 1);
        box_close(&xm, m_stbl);
        box_close(&xm, m_minf);
        box_close(&xm, m_mdia);
        box_close(&xm, m_trak);
        box_close(&xm, m);
        put_ftyp(&in);
        buf_32(&in, 12); buf_put(&in, "mdat", 4); buf_32(&in, 0);
        buf_put(&in, xm.p, xm.n);
        free(xm.p);
    }
    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.w_calls == 0);
    free(in.p); in.p = 0; in.n = in.cap = 0;
    io_free(&io);

    free(moov.p);
    return 0;
}

/************************************************************************/
/*          offsets group                                               */
/************************************************************************/

/* rewrite a real file and verify it with an independent box walk and
   MP4D_open/MP4D_frame_offset sample-by-sample against the source */
static int check_real_file(const char *path)
{
    uint64_t in_n = 0;
    unsigned char *in = slurp(path, &in_n);
    io_t io;
    int rc, n, i, t, nt, have_mdat = 0, have_free = 0;
    box_t boxes[512];
    tab_t tabs[64];
    tab_scan_t sc;
    MP4D_demux_t di, dm;
    blob_t bi, bo;

    TASK_CHECK(in != 0);
    io_init(&io, in, in_n);
    rc = MP4E_faststart(in_n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    TASK_CHECK(!io.r_bad && !io.w_bad);

    n = walk_boxes(io.out, io.out_n, boxes, 512);
    TASK_CHECK(n >= 3 && n <= 512);
    TASK_CHECK(boxes[0].type == type4("ftyp"));
    TASK_CHECK(boxes[1].type == type4("moov"));
    for (i = 2; i < n; i++)
    {
        if (boxes[i].type == type4("mdat")) have_mdat++;
        if (boxes[i].type == type4("free") || boxes[i].type == type4("skip")) have_free++;
    }
    TASK_CHECK(have_mdat >= 1);
    TASK_CHECK(have_free == 0);

    /* every chunk table entry lands inside an output mdat payload */
    memset(&sc, 0, sizeof(sc));
    sc.tabs = tabs; sc.max = 64;
    TASK_CHECK(scan_tables(io.out, boxes[1].off + boxes[1].hdr,
        boxes[1].size - boxes[1].hdr, &sc, 0) == 0);
    nt = sc.count;
    TASK_CHECK(nt >= 1);
    for (t = 0; t < nt && t < 64; t++)
    {
        uint32_t count = t_be32(io.out + tabs[t].coff + 4);
        int is64 = tabs[t].type == type4("co64");
        uint32_t e;
        TASK_CHECK(tabs[t].csize == 8 + (uint64_t)count * (is64 ? 8 : 4));
        for (e = 0; e < count; e++)
        {
            uint64_t v = is64 ? t_be64(io.out + tabs[t].coff + 8 + (uint64_t)e * 8)
                              : t_be32(io.out + tabs[t].coff + 8 + (uint64_t)e * 4);
            int ok = 0;
            for (i = 0; i < n; i++)
            {
                uint64_t pl;
                if (boxes[i].type != type4("mdat")) continue;
                pl = boxes[i].off + boxes[i].hdr;
                if (v >= pl && v < pl + (boxes[i].size - boxes[i].hdr)) ok = 1;
            }
            TASK_CHECK(ok);
        }
    }

    /* per-sample payload and timing comparison through the demuxer */
    bi.p = in; bi.n = in_n;
    bo.p = io.out; bo.n = io.out_n;
    TASK_CHECK(MP4D_open(&di, blob_read, &bi, (int64_t)in_n));
    TASK_CHECK(MP4D_open(&dm, blob_read, &bo, (int64_t)io.out_n));
    TASK_CHECK(di.track_count == dm.track_count);
    TASK_CHECK(di.track_count > 0);
    for (t = 0; t < (int)di.track_count; t++)
    {
        unsigned s;
        int ns1, ns2, np1, np2;
        const void *s1, *s2, *p1, *p2;
        TASK_CHECK(di.track[t].sample_count == dm.track[t].sample_count);
        TASK_CHECK(di.track[t].sample_count > 0);
        for (s = 0; s < di.track[t].sample_count; s++)
        {
            unsigned ni, tsi, dui, no, tso, duo;
            MP4D_file_offset_t oi = MP4D_frame_offset(&di, t, s, &ni, &tsi, &dui);
            MP4D_file_offset_t oo = MP4D_frame_offset(&dm, t, s, &no, &tso, &duo);
            TASK_CHECK(ni > 0 && ni == no);
            TASK_CHECK(tsi == tso && dui == duo);
            TASK_CHECK((uint64_t)oi + ni <= in_n && (uint64_t)oo + no <= io.out_n);
            TASK_CHECK(!memcmp(in + oi, io.out + oo, ni));
        }
        s1 = MP4D_read_sps(&di, t, 0, &ns1);
        s2 = MP4D_read_sps(&dm, t, 0, &ns2);
        TASK_CHECK((s1 != 0) == (s2 != 0));
        if (s1 && s2) TASK_CHECK(ns1 == ns2 && !memcmp(s1, s2, (size_t)ns1));
        p1 = MP4D_read_pps(&di, t, 0, &np1);
        p2 = MP4D_read_pps(&dm, t, 0, &np2);
        TASK_CHECK((p1 != 0) == (p2 != 0));
        if (p1 && p2) TASK_CHECK(np1 == np2 && !memcmp(p1, p2, (size_t)np1));
    }
    MP4D_close(&di);
    MP4D_close(&dm);

    free(in);
    io_free(&io);
    return 0;
}

static int case_offsets_real_default(void)
{
    return check_real_file("vectors/out_ref.mp4");
}

static int case_offsets_real_sequential(void)
{
    return check_real_file("vectors/out_sequential_ref.mp4");
}

/* two mdat boxes, an intervening free box, an existing co64 table, a self-
   contained dref, a udta leaf and an unknown top-level box */
static int case_offsets_multi_mdat(void)
{
    buf_t in = { 0 }, moov = { 0 };
    io_t io;
    size_t m;
    uint64_t p1, p2, q1, q2;
    uint32_t e_stco[2];
    uint64_t e_co64[1];
    box_t boxes[8];
    tab_t tabs[4];
    tab_scan_t sc;
    int rc, n, i;
    unsigned char pat1[40], pat2[60], udta_pat[16];

    for (i = 0; i < 40; i++) pat1[i] = (unsigned char)(i ^ 0x5a);
    for (i = 0; i < 60; i++) pat2[i] = (unsigned char)(i * 3 + 1);
    for (i = 0; i < 16; i++) udta_pat[i] = (unsigned char)(0x80 + i);

    put_ftyp(&in);
    p1 = in.n + 8;
    buf_32(&in, 48); buf_put(&in, "mdat", 4); buf_put(&in, pat1, 40);
    buf_32(&in, 8); buf_put(&in, "free", 4);
    p2 = in.n + 8;
    buf_32(&in, 68); buf_put(&in, "mdat", 4); buf_put(&in, pat2, 60);

    e_stco[0] = (uint32_t)(p1 + 5);     /* chunk of trak1 in mdat1 */
    e_stco[1] = (uint32_t)(p2 + 7);     /* chunk of trak1 in mdat2 */
    e_co64[0] = p2 + 30;                /* chunk of trak2 in mdat2 */
    box_open(&moov, "moov", &m);
    put_trak(&moov, 1, e_stco, 2, 0);   /* with self-contained dref */
    put_trak(&moov, 0, e_co64, 1, 1);
    buf_32(&moov, 24); buf_put(&moov, "udta", 4); buf_put(&moov, udta_pat, 16);
    box_close(&moov, m);
    buf_put(&in, moov.p, moov.n);
    buf_32(&in, 12); buf_put(&in, "zzzz", 4); buf_32(&in, 0xcafebabe);

    io_init(&io, in.p, in.n);
    rc = MP4E_faststart(in.n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    TASK_CHECK(!io.r_bad && !io.w_bad);

    n = walk_boxes(io.out, io.out_n, boxes, 8);
    TASK_CHECK(n == 5);
    TASK_CHECK(boxes[0].type == type4("ftyp"));
    TASK_CHECK(boxes[1].type == type4("moov"));
    TASK_CHECK(boxes[2].type == type4("mdat"));
    TASK_CHECK(boxes[3].type == type4("mdat"));
    TASK_CHECK(boxes[4].type == type4("zzzz"));

    /* mdat payloads copied byte-for-byte */
    q1 = boxes[2].off + 8;
    q2 = boxes[3].off + 8;
    TASK_CHECK(boxes[2].size == 48 && boxes[3].size == 68);
    TASK_CHECK(!memcmp(io.out + q1, pat1, 40));
    TASK_CHECK(!memcmp(io.out + q2, pat2, 60));
    /* different mdats shift by different deltas: no single uniform constant */
    TASK_CHECK(q1 - p1 != q2 - p2);
    /* unknown top-level box preserved */
    TASK_CHECK(t_be32(io.out + boxes[4].off + 8) == 0xcafebabe);

    memset(&sc, 0, sizeof(sc));
    sc.tabs = tabs; sc.max = 4;
    TASK_CHECK(scan_tables(io.out, boxes[1].off + 8, boxes[1].size - 8, &sc, 0) == 0);
    TASK_CHECK(sc.count == 2);
    /* trak1 stco: same 32-bit form, same entry order, per-mdat mapping */
    TASK_CHECK(tabs[0].type == type4("stco"));
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 4) == 2);
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 8) == q1 + 5);
    TASK_CHECK(t_be32(io.out + tabs[0].coff + 12) == q2 + 7);
    /* trak2 co64: stays 64-bit */
    TASK_CHECK(tabs[1].type == type4("co64"));
    TASK_CHECK(t_be32(io.out + tabs[1].coff + 4) == 1);
    TASK_CHECK(t_be64(io.out + tabs[1].coff + 8) == q2 + 30);
    /* non-offset leaf boxes survive byte-for-byte */
    {
        size_t u = find_nth(io.out + boxes[1].off, (size_t)boxes[1].size, "udta", 0);
        TASK_CHECK(u != (size_t)-1);
        TASK_CHECK(!memcmp(io.out + boxes[1].off + u + 4, udta_pat, 16));
    }

    free(in.p); free(moov.p); io_free(&io);
    return 0;
}

/* faststart of an already-faststart file is a fixed point */
static int case_offsets_already_faststart(void)
{
    uint64_t in_n = 0;
    unsigned char *in = slurp("vectors/out_ref.mp4", &in_n);
    io_t io1, io2;
    int rc;
    box_t boxes[8];
    int n;

    TASK_CHECK(in != 0);
    io_init(&io1, in, in_n);
    rc = MP4E_faststart(in_n, &io1, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    n = walk_boxes(io1.out, io1.out_n, boxes, 8);
    TASK_CHECK(n >= 3 && boxes[1].type == type4("moov"));   /* already faststart */

    io_init(&io2, io1.out, io1.out_n);
    rc = MP4E_faststart(io1.out_n, &io2, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    TASK_CHECK(!io2.r_bad && !io2.w_bad);
    TASK_CHECK(io2.out_n == io1.out_n);
    TASK_CHECK(!memcmp(io2.out, io1.out, (size_t)io1.out_n));

    /* and the result still demuxes like the original */
    {
        MP4D_demux_t di, dm;
        blob_t bi, bo;
        unsigned s, count;
        bi.p = in; bi.n = in_n;
        bo.p = io2.out; bo.n = io2.out_n;
        TASK_CHECK(MP4D_open(&di, blob_read, &bi, (int64_t)in_n));
        TASK_CHECK(MP4D_open(&dm, blob_read, &bo, (int64_t)io2.out_n));
        TASK_CHECK(di.track_count == dm.track_count);
        count = di.track[0].sample_count;
        TASK_CHECK(count == dm.track[0].sample_count && count > 0);
        for (s = 0; s < count; s++)
        {
            unsigned ni, tsi, dui, no, tso, duo;
            MP4D_file_offset_t oi = MP4D_frame_offset(&di, 0, s, &ni, &tsi, &dui);
            MP4D_file_offset_t oo = MP4D_frame_offset(&dm, 0, s, &no, &tso, &duo);
            TASK_CHECK(ni == no && tsi == tso && dui == duo);
            TASK_CHECK(!memcmp(in + oi, io2.out + oo, ni));
        }
        MP4D_close(&di);
        MP4D_close(&dm);
    }

    free(in);
    io_free(&io1);
    io_free(&io2);
    return 0;
}

/************************************************************************/
/*          stream group                                                */
/************************************************************************/

/* normal copy: write offsets are monotone from 0, chunk sizes bounded,
   mdat payload delivered byte-for-byte */
static int case_stream_bounds_monotone(void)
{
    uint64_t in_n = 0;
    unsigned char *in = slurp("vectors/out_ref.mp4", &in_n);
    io_t io;
    int rc, n_in, n_out, i;
    box_t bi[8], bo[8];

    TASK_CHECK(in != 0);
    io_init(&io, in, in_n);
    rc = MP4E_faststart(in_n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);
    TASK_CHECK(!io.r_bad && !io.w_bad);
    TASK_CHECK(io.r_max > 0 && io.r_max <= 65536);
    TASK_CHECK(io.w_max > 0 && io.w_max <= 65536);
    TASK_CHECK(io.w_expect == io.out_n);
    TASK_CHECK(io.r_calls > 3 && io.w_calls > 3);

    n_in = walk_boxes(in, in_n, bi, 8);
    n_out = walk_boxes(io.out, io.out_n, bo, 8);
    TASK_CHECK(n_in >= 3 && n_out >= 3);
    /* mdat payload identical between input and output */
    {
        const box_t *mi = 0, *mo = 0;
        for (i = 0; i < n_in; i++) if (bi[i].type == type4("mdat")) mi = &bi[i];
        for (i = 0; i < n_out; i++) if (bo[i].type == type4("mdat")) mo = &bo[i];
        TASK_CHECK(mi && mo);
        TASK_CHECK(mi->size == mo->size);
        TASK_CHECK(!memcmp(in + mi->off, io.out + mo->off, (size_t)mi->size));
    }

    free(in);
    io_free(&io);
    return 0;
}

/* read callback failures abort the rewrite with MP4E_STATUS_FILE_WRITE_ERROR */
static int case_stream_read_failure(void)
{
    uint64_t in_n = 0;
    unsigned char *in = slurp("vectors/out_ref.mp4", &in_n);
    io_t io, io2, io3;
    int rc;

    TASK_CHECK(in != 0);
    io_init(&io, in, in_n);
    rc = MP4E_faststart(in_n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);

    /* failure during the planning phase: nothing is written at all */
    io_init(&io2, in, in_n);
    io2.r_fail_call = 1;
    rc = MP4E_faststart(in_n, &io2, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io2.w_calls == 0 && io2.out_n == 0);

    /* failure on the last read (mid copy phase): partial output, then stop */
    io_init(&io3, in, in_n);
    io3.r_fail_call = io.r_calls;
    rc = MP4E_faststart(in_n, &io3, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io3.w_calls > 0);
    TASK_CHECK(io3.w_calls < io.w_calls);
    TASK_CHECK(io3.out_n < io.out_n);

    free(in);
    io_free(&io);
    io_free(&io2);
    io_free(&io3);
    return 0;
}

/* write callback failures abort the rewrite and nothing more is delivered */
static int case_stream_write_failure(void)
{
    uint64_t in_n = 0;
    unsigned char *in = slurp("vectors/out_ref.mp4", &in_n);
    io_t io, io2;
    int rc;

    TASK_CHECK(in != 0);
    io_init(&io, in, in_n);
    rc = MP4E_faststart(in_n, &io, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_OK);

    io_init(&io2, in, in_n);
    io2.w_fail_call = 2;
    rc = MP4E_faststart(in_n, &io2, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io2.w_calls == 2);            /* no further writes after the failure */
    TASK_CHECK(io2.out_n < io.out_n);

    io_free(&io);
    io_free(&io2);

    io_init(&io2, in, in_n);
    io2.w_fail_call = 1;
    rc = MP4E_faststart(in_n, &io2, io_read, io_write);
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io2.w_calls == 1);
    TASK_CHECK(io2.out_n == 0);

    free(in);
    io_free(&io2);
    return 0;
}

/************************************************************************/
/*          registration                                                */
/************************************************************************/

const struct task_case task_cases[] = {
    { "layout", "order_free_skip", case_layout_order_free_skip },
    { "layout", "extended_headers", case_layout_extended_headers },
    { "layout", "64bit_cascade", case_layout_64bit_cascade },
    { "layout", "bad_inputs", case_layout_bad_inputs },
    { "offsets", "real_default", case_offsets_real_default },
    { "offsets", "real_sequential", case_offsets_real_sequential },
    { "offsets", "multi_mdat", case_offsets_multi_mdat },
    { "offsets", "already_faststart", case_offsets_already_faststart },
    { "stream", "bounds_monotone", case_stream_bounds_monotone },
    { "stream", "read_failure", case_stream_read_failure },
    { "stream", "write_failure", case_stream_write_failure },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
