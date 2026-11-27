/*
    Tests for MP4E_faststart(): fast-start (moov-before-mdat) rewrite of
    existing non-fragmented MP4 files.  No main() here; the harness in
    task_support/driver.c runs task_cases[].
*/
#include "minimp4.h"
#include "task_support/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------- big-endian helpers ---------------- */
static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64(const unsigned char *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}
static void wr64(unsigned char *p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v);
}
static uint32_t fourcc(const char s[4])
{
    return ((uint32_t)(unsigned char)s[0] << 24) | ((uint32_t)(unsigned char)s[1] << 16) |
           ((uint32_t)(unsigned char)s[2] << 8) | (uint32_t)(unsigned char)s[3];
}

/* ---------------- in-memory input + instrumented callbacks ---------------- */
typedef struct { unsigned char *p; size_t n; } mem_t;

static int mem_read(int64_t off, void *dst, size_t n, void *tok)
{
    mem_t *m = (mem_t *)tok;
    if (off < 0 || (uint64_t)off > m->n || n > m->n - (size_t)off)
        return 1;
    memcpy(dst, m->p + (size_t)off, n);
    return 0;
}

typedef struct
{
    mem_t in;
    /* output sink */
    unsigned char *op;
    size_t on, ocap;
    int64_t expect;         /* next expected write offset */
    int contiguous;         /* all writes contiguous from 0 so far */
    size_t max_write;       /* largest single write */
    unsigned writes;
    int64_t fail_at;        /* fail writes with offset >= this (-1: never) */
    int write_failed;
    int write_after_fail;   /* library called write again after a failure */
    /* read instrumentation */
    unsigned reads;
    size_t max_read;
    long fail_read_after;   /* fail the (n+1)-th read (-1: never) */
    int read_failed;
} io_t;

static void io_init(io_t *io)
{
    memset(io, 0, sizeof(*io));
    io->expect = 0;
    io->contiguous = 1;
    io->fail_at = -1;
    io->fail_read_after = -1;
}

static int io_read(int64_t off, void *dst, size_t n, void *tok)
{
    io_t *io = (io_t *)tok;
    if (io->fail_read_after >= 0 && (long)io->reads >= io->fail_read_after)
    {
        io->read_failed = 1;
        return 1;
    }
    io->reads++;
    if (n > io->max_read)
        io->max_read = n;
    return mem_read(off, dst, n, &io->in);
}

static int io_write(int64_t off, const void *src, size_t n, void *tok)
{
    io_t *io = (io_t *)tok;
    if (io->write_failed)
    {
        io->write_after_fail = 1;
        return 1;
    }
    if (off != io->expect)
        io->contiguous = 0;
    if (n > io->max_write)
        io->max_write = n;
    io->writes++;
    if (io->fail_at >= 0 && off >= io->fail_at)
    {
        io->write_failed = 1;
        return 1;
    }
    if (io->on + n > io->ocap)
    {
        size_t c = io->ocap ? io->ocap * 2 : 1 << 16;
        while (c < io->on + n)
            c *= 2;
        io->op = (unsigned char *)realloc(io->op, c);
        if (!io->op)
            exit(9);
        io->ocap = c;
    }
    memcpy(io->op + io->on, src, n);
    io->on += n;
    io->expect = off + (int64_t)n;
    return 0;
}

static int load_file(const char *path, mem_t *m)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END))
    {
        fclose(f);
        return 0;
    }
    m->n = (size_t)ftell(f);
    rewind(f);
    m->p = (unsigned char *)malloc(m->n);
    if (!m->p || fread(m->p, 1, m->n, f) != m->n)
    {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

/* ---------------- independent box reader ---------------- */
typedef struct { uint32_t type; uint64_t pos, size; int hdr; } box_t;

static int box_header_at(const unsigned char *d, uint64_t pos, uint64_t end,
                         uint32_t *type, uint64_t *size, int *hdr)
{
    uint32_t s32;
    if (end - pos < 8)
        return -1;
    s32 = rd32(d + pos);
    *type = rd32(d + pos + 4);
    if (s32 == 1)
    {
        if (end - pos < 16)
            return -1;
        *size = rd64(d + pos + 8);
        *hdr = 16;
    } else if (s32 == 0)
    {
        return -1;
    } else
    {
        *size = s32;
        *hdr = 8;
    }
    if (*size < (uint64_t)*hdr || *size > end - pos)
        return -1;
    return 0;
}

/* parse top-level boxes; returns count (<= max stored in out) or -1 */
static int read_boxes(const unsigned char *d, size_t n, box_t *out, int max)
{
    uint64_t pos = 0;
    int count = 0;
    while (pos < n)
    {
        uint32_t type;
        uint64_t size;
        int hdr;
        if (box_header_at(d, pos, n, &type, &size, &hdr))
            return -1;
        if (count < max)
        {
            out[count].type = type;
            out[count].pos = pos;
            out[count].size = size;
            out[count].hdr = hdr;
        }
        count++;
        pos += size;
    }
    return count;
}

/* walk moov/trak/mdia/minf/stbl of buffer d, collecting stco/co64 tables */
typedef struct { uint32_t type; uint32_t count; uint64_t ent[8]; } tab_t;

static void walk_tabs(const unsigned char *d, uint64_t start, uint64_t end, int depth,
                      tab_t *tabs, int *ntab, int max)
{
    uint64_t pos = start;
    while (pos < end)
    {
        uint32_t type;
        uint64_t size;
        int hdr;
        if (box_header_at(d, pos, end, &type, &size, &hdr))
            return;
        if (depth < 4)
        {
            uint32_t want = depth == 0 ? fourcc("trak") : depth == 1 ? fourcc("mdia") :
                            depth == 2 ? fourcc("minf") : fourcc("stbl");
            if (type == want)
                walk_tabs(d, pos + hdr, pos + size, depth + 1, tabs, ntab, max);
        } else if ((type == fourcc("stco") || type == fourcc("co64")) && *ntab < max)
        {
            tab_t *t = tabs + (*ntab)++;
            const unsigned char *p = d + pos + hdr;
            uint32_t i, count = rd32(p + 4);
            int c64 = (type == fourcc("co64"));
            t->type = type;
            t->count = count;
            for (i = 0; i < count && i < 8; i++)
                t->ent[i] = c64 ? rd64(p + 8 + 8 * i) : rd32(p + 8 + 4 * i);
        }
        pos += size;
    }
}

/* ---------------- synthetic file builder ---------------- */
typedef struct { unsigned char *p; size_t n, cap; } bld_t;

static void bput(bld_t *b, const void *src, size_t n)
{
    if (b->n + n > b->cap)
    {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->n + n)
            c *= 2;
        b->p = (unsigned char *)realloc(b->p, c);
        if (!b->p)
            exit(9);
        b->cap = c;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static void b8(bld_t *b, int v)
{
    unsigned char c = (unsigned char)v;
    bput(b, &c, 1);
}
static void b32(bld_t *b, uint32_t v)
{
    unsigned char t[4];
    wr32(t, v);
    bput(b, t, 4);
}
static void b64(bld_t *b, uint64_t v)
{
    unsigned char t[8];
    wr64(t, v);
    bput(b, t, 8);
}
static void btag(bld_t *b, const char *s)
{
    bput(b, s, 4);
}
static size_t bbox(bld_t *b, const char *type)
{
    size_t at = b->n;
    b32(b, 0);
    btag(b, type);
    return at;
}
static void bend(bld_t *b, size_t at)
{
    wr32(b->p + at, (uint32_t)(b->n - at));
}

typedef struct
{
    int co64;               /* write this track's table as co64 */
    uint64_t offs[4];       /* chunk offsets */
    uint32_t sizes[4];      /* sample sizes, one per chunk */
    uint32_t n;             /* chunk/sample count */
} trak_spec_t;

static void b_trak(bld_t *b, const trak_spec_t *t, int track_id)
{
    size_t a = bbox(b, "trak"), x;
    uint32_t i;
    x = bbox(b, "tkhd");
    b32(b, 3);                  /* version/flags */
    b32(b, 0); b32(b, 0);       /* creation/modification time */
    b32(b, (uint32_t)track_id);
    b32(b, 0);                  /* reserved */
    b32(b, 4000);               /* duration */
    b32(b, 0); b32(b, 0);       /* reserved */
    b32(b, 0);                  /* layer + alternate group */
    b32(b, 0x01000000);         /* volume + reserved */
    b32(b, 0x00010000); b32(b, 0); b32(b, 0);
    b32(b, 0); b32(b, 0x00010000); b32(b, 0);
    b32(b, 0); b32(b, 0); b32(b, 0x40000000);
    b32(b, 320u << 16); b32(b, 240u << 16);
    bend(b, x);
    x = bbox(b, "mdia");
    {
        size_t y = bbox(b, "mdhd");
        b32(b, 0);              /* version/flags */
        b32(b, 0); b32(b, 0);   /* creation/modification time */
        b32(b, 1000);           /* timescale */
        b32(b, 4000);           /* duration */
        b32(b, 0x55c40000);     /* language 'und' + pre_defined */
        bend(b, y);
        y = bbox(b, "hdlr");
        b32(b, 0);              /* version/flags */
        b32(b, 0);              /* pre_defined */
        btag(b, "vide");
        b32(b, 0); b32(b, 0); b32(b, 0);
        b8(b, 0);
        bend(b, y);
        y = bbox(b, "minf");
        {
            size_t z = bbox(b, "vmhd");
            b32(b, 1);          /* version/flags */
            b32(b, 0); b32(b, 0);
            bend(b, z);
            z = bbox(b, "dinf");
            {
                size_t w = bbox(b, "dref");
                b32(b, 0);      /* version/flags */
                b32(b, 1);      /* entry_count */
                {
                    size_t u = bbox(b, "url ");
                    b32(b, 1);  /* self-contained */
                    bend(b, u);
                }
                bend(b, w);
            }
            bend(b, z);
            z = bbox(b, "stbl");
            {
                size_t w = bbox(b, "stsd");
                b32(b, 0);      /* version/flags */
                b32(b, 1);      /* entry_count */
                {
                    size_t u = bbox(b, "gnrv");
                    b32(b, 0); b8(b, 0); b8(b, 0);  /* reserved[6] */
                    b8(b, 0); b8(b, 1);             /* data_reference_index */
                    b32(b, 0); b32(b, 0);
                    bend(b, u);
                }
                bend(b, w);
                w = bbox(b, "stts");
                b32(b, 0); b32(b, 1);
                b32(b, t->n); b32(b, 1000);
                bend(b, w);
                w = bbox(b, "stsc");
                b32(b, 0); b32(b, 1);
                b32(b, 1); b32(b, 1); b32(b, 1);
                bend(b, w);
                w = bbox(b, "stsz");
                b32(b, 0); b32(b, 0); b32(b, t->n);
                for (i = 0; i < t->n; i++)
                    b32(b, t->sizes[i]);
                bend(b, w);
                w = bbox(b, t->co64 ? "co64" : "stco");
                b32(b, 0); b32(b, t->n);
                for (i = 0; i < t->n; i++)
                {
                    if (t->co64)
                        b64(b, t->offs[i]);
                    else
                        b32(b, (uint32_t)t->offs[i]);
                }
                bend(b, w);
            }
            bend(b, z);
        }
        bend(b, y);
    }
    bend(b, x);
    bend(b, a);
}

/* append a moov box with the given tracks and an optional in-moov free pad */
static void b_moov(bld_t *b, const trak_spec_t *traks, int ntrak, size_t pad)
{
    size_t a = bbox(b, "moov"), x;
    int i;
    x = bbox(b, "mvhd");
    b32(b, 0);                  /* version/flags */
    b32(b, 0); b32(b, 0);       /* creation/modification time */
    b32(b, 1000); b32(b, 4000); /* timescale + duration */
    b32(b, 0x00010000);         /* rate */
    b32(b, 0x01000000);         /* volume + reserved */
    b32(b, 0); b32(b, 0);       /* reserved */
    b32(b, 0x00010000); b32(b, 0); b32(b, 0);
    b32(b, 0); b32(b, 0x00010000); b32(b, 0);
    b32(b, 0); b32(b, 0); b32(b, 0x40000000);
    b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0);
    b32(b, 2);                  /* next_track_ID */
    bend(b, x);
    for (i = 0; i < ntrak; i++)
        b_trak(b, traks + i, i + 1);
    if (pad)
    {
        x = bbox(b, "free");
        for (i = 0; (size_t)i < pad; i++)
            b8(b, 0);
        bend(b, x);
    }
    bend(b, a);
}

/* synthetic file: ftyp, free, mdat1, unknown 'abcd', free, mdat2, skip, moov.
   Track 1 uses stco with chunks in both mdats, track 2 uses co64. */
typedef struct
{
    mem_t file;
    uint64_t mdat1_payload, mdat2_payload;
    uint64_t c10, c11, c20;     /* chunk offsets in the input */
    uint64_t abcd_pos, abcd_size;
} synth_t;

static void build_synthetic(synth_t *s)
{
    bld_t b = { NULL, 0, 0 };
    size_t x;
    int i;
    x = bbox(&b, "ftyp");
    btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
    bend(&b, x);
    x = bbox(&b, "free");
    b32(&b, 0); b32(&b, 0);
    bend(&b, x);
    x = bbox(&b, "mdat");
    s->mdat1_payload = b.n;
    for (i = 0; i < 100; i++)
        b8(&b, 0xA0 + (i & 0xF));
    bend(&b, x);
    x = bbox(&b, "abcd");
    s->abcd_pos = b.n - 8;
    for (i = 0; i < 20; i++)
        b8(&b, 0xC0 + i);
    bend(&b, x);
    s->abcd_size = b.n - s->abcd_pos;
    x = bbox(&b, "free");
    b32(&b, 0xdeadbeef);
    bend(&b, x);
    x = bbox(&b, "mdat");
    s->mdat2_payload = b.n;
    for (i = 0; i < 60; i++)
        b8(&b, 0xB0 + (i & 0xF));
    bend(&b, x);
    x = bbox(&b, "skip");
    bend(&b, x);
    {
        trak_spec_t t[2];
        memset(t, 0, sizeof(t));
        s->c10 = s->mdat1_payload;          /* 40-byte sample */
        s->c20 = s->mdat1_payload + 50;     /* 30-byte sample */
        s->c11 = s->mdat2_payload + 10;     /* 30-byte sample */
        t[0].co64 = 0; t[0].n = 2;
        t[0].offs[0] = s->c10; t[0].sizes[0] = 40;
        t[0].offs[1] = s->c11; t[0].sizes[1] = 30;
        t[1].co64 = 1; t[1].n = 1;
        t[1].offs[0] = s->c20; t[1].sizes[0] = 30;
        b_moov(&b, t, 2, 0);
    }
    s->file.p = b.p;
    s->file.n = b.n;
}

/* run MP4E_faststart over io->in into io->out */
static int run_fs(io_t *io)
{
    return MP4E_faststart(io->in.n, io, io_read, io_write);
}

/* compare every sample's payload bytes and timing between two files */
static int samples_match(mem_t *a, mem_t *b)
{
    MP4D_demux_t da, db;
    int ok = 1;
    unsigned t, s;
    memset(&da, 0, sizeof(da));
    memset(&db, 0, sizeof(db));
    if (!MP4D_open(&da, mem_read, a, (int64_t)a->n))
        return 0;
    if (!MP4D_open(&db, mem_read, b, (int64_t)b->n))
    {
        MP4D_close(&da);
        return 0;
    }
    ok &= (da.track_count == db.track_count);
    for (t = 0; ok && t < da.track_count; t++)
    {
        int n1 = 0, n2 = 0;
        const void *sps1 = MP4D_read_sps(&da, t, 0, &n1);
        const void *sps2 = MP4D_read_sps(&db, t, 0, &n2);
        ok &= (da.track[t].sample_count == db.track[t].sample_count);
        if (sps1 && sps2)
            ok &= (n1 == n2) && !memcmp(sps1, sps2, (size_t)n1);
        else
            ok &= (sps1 == sps2);
        for (s = 0; ok && s < da.track[t].sample_count; s++)
        {
            unsigned nb1 = 0, nb2 = 0, ts1 = 0, ts2 = 0, dur1 = 0, dur2 = 0;
            MP4D_file_offset_t o1 = MP4D_frame_offset(&da, t, s, &nb1, &ts1, &dur1);
            MP4D_file_offset_t o2 = MP4D_frame_offset(&db, t, s, &nb2, &ts2, &dur2);
            ok &= (nb1 == nb2) && (ts1 == ts2) && (dur1 == dur2);
            ok &= ((uint64_t)o1 + nb1 <= a->n) && ((uint64_t)o2 + nb2 <= b->n);
            if (ok)
                ok &= !memcmp(a->p + (size_t)o1, b->p + (size_t)o2, nb1);
        }
    }
    MP4D_close(&da);
    MP4D_close(&db);
    return ok;
}

/* ---------------- layout group ---------------- */

static int case_layout_default_real(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    box_t ib[8], ob[8];
    int ni, mo;
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    ni = read_boxes(in.p, in.n, ib, 8);
    TASK_CHECK(ni == 4);    /* ftyp, free, mdat, moov */
    io_init(&io);
    io.in = in;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    TASK_CHECK(read_boxes(io.op, io.on, ob, 8) == 3);
    TASK_CHECK(ob[0].type == fourcc("ftyp"));
    TASK_CHECK(ob[1].type == fourcc("moov"));
    TASK_CHECK(ob[2].type == fourcc("mdat"));
    /* ftyp and mdat preserved byte-for-byte */
    TASK_CHECK(ob[0].size == ib[0].size && !memcmp(io.op, in.p, (size_t)ib[0].size));
    TASK_CHECK(ob[2].size == ib[2].size);
    TASK_CHECK(!memcmp(io.op + ob[2].pos, in.p + ib[2].pos, (size_t)ib[2].size));
    /* no top-level free/skip survives */
    TASK_CHECK(io.on == ob[0].size + ob[1].size + ob[2].size);
    /* result opens as a valid movie */
    mo = 0;
    {
        MP4D_demux_t d;
        memset(&d, 0, sizeof(d));
        mo = MP4D_open(&d, mem_read, (mem_t[]){ { io.op, io.on } }, (int64_t)io.on);
        TASK_CHECK(mo);
        TASK_CHECK(d.track_count == 1 && d.track[0].sample_count > 1);
        MP4D_close(&d);
    }
    free(in.p);
    free(io.op);
    return 0;
}

static int case_layout_sequential_real(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    box_t *ib, *ob;
    int ni, no, i, nm = 0;
    TASK_CHECK(load_file("vectors/out_sequential_ref.mp4", &in));
    ib = (box_t *)malloc(1024 * sizeof(box_t));
    ob = (box_t *)malloc(1024 * sizeof(box_t));
    TASK_CHECK(ib && ob);
    ni = read_boxes(in.p, in.n, ib, 1024);
    TASK_CHECK(ni > 3);
    for (i = 0; i < ni; i++)
        if (ib[i].type == fourcc("mdat"))
            nm++;
    TASK_CHECK(nm > 100);   /* sequential mode: one mdat per sample */
    io_init(&io);
    io.in = in;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    no = read_boxes(io.op, io.on, ob, 1024);
    TASK_CHECK(no == nm + 2);
    TASK_CHECK(ob[0].type == fourcc("ftyp"));
    TASK_CHECK(ob[1].type == fourcc("moov"));
    /* every mdat keeps its relative order and bytes */
    {
        int k = 0, ok = 1;
        for (i = 0; i < ni; i++)
            if (ib[i].type == fourcc("mdat"))
            {
                ok &= (ob[2 + k].type == fourcc("mdat"));
                ok &= (ob[2 + k].size == ib[i].size);
                ok &= !memcmp(io.op + ob[2 + k].pos, in.p + ib[i].pos, (size_t)ib[i].size);
                k++;
            }
        TASK_CHECK(ok);
    }
    TASK_CHECK(io.on == ob[no - 1].pos + ob[no - 1].size);
    free(ib);
    free(ob);
    free(in.p);
    free(io.op);
    return 0;
}

static int case_layout_synthetic_multi_mdat(void)
{
    synth_t s;
    io_t io;
    box_t ob[8];
    tab_t tabs[4];
    int nt = 0;
    build_synthetic(&s);
    io_init(&io);
    io.in = s.file;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    TASK_CHECK(read_boxes(io.op, io.on, ob, 8) == 5);
    /* ftyp, new moov, then the rest in original relative order, free/skip gone */
    TASK_CHECK(ob[0].type == fourcc("ftyp"));
    TASK_CHECK(ob[1].type == fourcc("moov"));
    TASK_CHECK(ob[2].type == fourcc("mdat"));
    TASK_CHECK(ob[3].type == fourcc("abcd"));
    TASK_CHECK(ob[4].type == fourcc("mdat"));
    /* unknown box content preserved verbatim */
    TASK_CHECK(ob[3].size == s.abcd_size);
    TASK_CHECK(!memcmp(io.op + ob[3].pos, s.file.p + s.abcd_pos, (size_t)s.abcd_size));
    /* existing co64 table is not downgraded, stco stays stco */
    walk_tabs(io.op, ob[1].pos + ob[1].hdr, ob[1].pos + ob[1].size, 0, tabs, &nt, 4);
    TASK_CHECK(nt == 2);
    TASK_CHECK(tabs[0].type == fourcc("stco") && tabs[0].count == 2);
    TASK_CHECK(tabs[1].type == fourcc("co64") && tabs[1].count == 1);
    free(s.file.p);
    free(io.op);
    return 0;
}

static int case_layout_already_faststart(void)
{
    mem_t in = { NULL, 0 };
    io_t pass1, pass2;
    box_t ob[8];
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    io_init(&pass1);
    pass1.in = in;
    TASK_CHECK(run_fs(&pass1) == MP4E_STATUS_OK);
    /* feed the already-faststart output through the rewriter again */
    io_init(&pass2);
    pass2.in.p = pass1.op;
    pass2.in.n = pass1.on;
    TASK_CHECK(run_fs(&pass2) == MP4E_STATUS_OK);
    TASK_CHECK(read_boxes(pass2.op, pass2.on, ob, 8) == 3);
    TASK_CHECK(ob[0].type == fourcc("ftyp"));
    TASK_CHECK(ob[1].type == fourcc("moov"));
    TASK_CHECK(ob[2].type == fourcc("mdat"));
    /* samples still intact after the double rewrite */
    {
        mem_t out2 = { pass2.op, pass2.on };
        TASK_CHECK(samples_match(&in, &out2));
    }
    free(in.p);
    free(pass1.op);
    free(pass2.op);
    return 0;
}

/* Virtual ~4.3 GB input: two stco tables, the second one crosses UINT32_MAX
   only after the first table's stco->co64 upgrade grows the moov.  The write
   callback stops the stream once the whole new moov has been delivered, so
   no gigabytes of media are ever moved. */
#define CASC_T 0xFFFFF000ull    /* input mdat2 payload start */

typedef struct
{
    unsigned char hdr1[16], hdr2[16];
    const unsigned char *moov;
    size_t moov_size;
    uint64_t mdat1_pos, mdat2_pos, moov_pos;
} virt_t;

static int virt_read(int64_t off, void *dst, size_t n, void *tok)
{
    virt_t *v = (virt_t *)tok;
    unsigned char *d = (unsigned char *)dst;
    size_t i;
    static const unsigned char ftyp[24] = {
        0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm',
        0, 0, 0, 0, 'i', 's', 'o', 'm', 'i', 's', 'o', '2'
    };
    for (i = 0; i < n; i++)
    {
        uint64_t o = (uint64_t)off + i;
        if (o < 24)
            d[i] = ftyp[o];
        else if (o >= v->mdat1_pos && o < v->mdat1_pos + 16)
            d[i] = v->hdr1[o - v->mdat1_pos];
        else if (o >= v->mdat2_pos && o < v->mdat2_pos + 16)
            d[i] = v->hdr2[o - v->mdat2_pos];
        else if (o >= v->moov_pos && o < v->moov_pos + v->moov_size)
            d[i] = v->moov[o - v->moov_pos];
        else
            d[i] = 0;
    }
    return 0;
}

typedef struct
{
    unsigned char *p;
    size_t n, cap;
    int64_t expect;
    int contiguous;
    uint64_t limit;     /* fail any write at/after this output offset */
    int failed;
} csink_t;

static int csink_write(int64_t off, const void *src, size_t n, void *tok)
{
    csink_t *s = (csink_t *)tok;
    if (off != s->expect)
        s->contiguous = 0;
    if ((uint64_t)off >= s->limit)
    {
        s->failed = 1;
        return 1;
    }
    if (s->n + n > s->cap)
    {
        size_t c = s->cap ? s->cap * 2 : 4096;
        while (c < s->n + n)
            c *= 2;
        s->p = (unsigned char *)realloc(s->p, c);
        if (!s->p)
            exit(9);
        s->cap = c;
    }
    memcpy(s->p + s->n, src, n);
    s->n += n;
    s->expect = off + (int64_t)n;
    return 0;
}

typedef struct { virt_t *v; csink_t *s; } casc_io_t;
static int casc_read(int64_t off, void *dst, size_t n, void *tok)
{
    return virt_read(off, dst, n, ((casc_io_t *)tok)->v);
}
static int casc_write(int64_t off, const void *src, size_t n, void *tok)
{
    return csink_write(off, src, n, ((casc_io_t *)tok)->s);
}

static int case_layout_cascade_64bit(void)
{
    bld_t mb = { NULL, 0, 0 };
    trak_spec_t t[2];
    uint64_t S0, c1 = 0xF00, c2, P1, P2 = 0x2000;
    uint64_t mdat1_pos, mdat1_size, mdat2_pos, mdat2_size, moov_pos, total;
    virt_t v;
    csink_t cs;
    casc_io_t cio;
    box_t ob[4];
    tab_t tabs[4];
    int nt = 0, rc;

    memset(t, 0, sizeof(t));
    t[0].n = 1; t[0].sizes[0] = 16;
    t[1].n = 1; t[1].sizes[0] = 16;
    b_moov(&mb, t, 2, 128);     /* in-moov free pad keeps S0 in the wanted range */
    S0 = mb.n;
    TASK_CHECK(S0 > 0x100 && S0 < 0xF00);
    /* chunk 2 lands exactly on UINT32_MAX before any upgrade */
    c2 = 0xFFF - S0;
    t[0].offs[0] = CASC_T + c1;
    t[1].offs[0] = CASC_T + c2;
    mb.n = 0;                   /* same sizes, real offsets */
    b_moov(&mb, t, 2, 128);
    TASK_CHECK(mb.n == S0);
    TASK_CHECK(t[0].offs[0] <= 0xFFFFFFFFull && t[1].offs[0] <= 0xFFFFFFFFull);
    /* boundary conditions of the cascade, precomputed */
    TASK_CHECK(S0 + CASC_T + c1 > 0xFFFFFFFFull);       /* table 1 overflows at once */
    TASK_CHECK(S0 + CASC_T + c2 == 0xFFFFFFFFull);      /* table 2 still fits */
    TASK_CHECK(S0 + 4 + CASC_T + c2 > 0xFFFFFFFFull);   /* ...until table 1 grows moov */

    P1 = CASC_T - 56;
    mdat1_pos = 24;
    mdat1_size = 16 + P1;
    mdat2_pos = mdat1_pos + mdat1_size;
    mdat2_size = 16 + P2;
    moov_pos = mdat2_pos + mdat2_size;
    total = moov_pos + S0;
    TASK_CHECK(total > 0x100000000ull && total <= 0x7FFFFFFFFFFFFFFFull);

    memset(&v, 0, sizeof(v));
    wr32(v.hdr1, 1); wr32(v.hdr1 + 4, fourcc("mdat")); wr64(v.hdr1 + 8, mdat1_size);
    wr32(v.hdr2, 1); wr32(v.hdr2 + 4, fourcc("mdat")); wr64(v.hdr2 + 8, mdat2_size);
    v.mdat1_pos = mdat1_pos;
    v.mdat2_pos = mdat2_pos;
    v.moov_pos = moov_pos;
    v.moov = mb.p;
    v.moov_size = mb.n;

    memset(&cs, 0, sizeof(cs));
    cs.expect = 0;
    cs.contiguous = 1;
    cs.limit = 24 + S0 + 8;     /* ftyp + moov with both tables upgraded to co64 */
    cio.v = &v;
    cio.s = &cs;

    rc = MP4E_faststart(total, &cio, casc_read, casc_write);
    TASK_CHECK(rc == MP4E_STATUS_FILE_WRITE_ERROR);     /* stopped by our writer */
    TASK_CHECK(cs.failed);
    TASK_CHECK(cs.contiguous);
    TASK_CHECK(cs.n == 24 + S0 + 8);                    /* ftyp + whole new moov only */
    TASK_CHECK(read_boxes(cs.p, cs.n, ob, 4) == 2);
    TASK_CHECK(ob[0].type == fourcc("ftyp") && ob[0].size == 24);
    TASK_CHECK(ob[1].type == fourcc("moov"));
    TASK_CHECK(ob[1].size == S0 + 8);                   /* +4 per upgraded entry */
    walk_tabs(cs.p, ob[1].pos + ob[1].hdr, ob[1].pos + ob[1].size, 0, tabs, &nt, 4);
    TASK_CHECK(nt == 2);
    TASK_CHECK(tabs[0].type == fourcc("co64") && tabs[0].count == 1);
    TASK_CHECK(tabs[1].type == fourcc("co64") && tabs[1].count == 1);
    /* new offset = new mdat2 payload position + in-payload delta */
    TASK_CHECK(tabs[0].ent[0] == ob[1].size + CASC_T + c1);
    TASK_CHECK(tabs[1].ent[0] == ob[1].size + CASC_T + c2);
    TASK_CHECK(tabs[0].ent[0] > 0xFFFFFFFFull);
    TASK_CHECK(tabs[1].ent[0] > 0xFFFFFFFFull);
    free(mb.p);
    free(cs.p);
    return 0;
}

static int case_layout_reject_malformed(void)
{
    io_t io;
    int rc;
    /* box with size == 0 ("till eof") is not supported */
    {
        unsigned char d[24];
        mem_t m = { d, sizeof(d) };
        wr32(d, 0); wr32(d + 4, fourcc("ftyp"));
        memset(d + 8, 0, 16);
        io_init(&io); io.in = m;
        rc = MP4E_faststart(m.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(io.writes == 0);
        free(io.op);
    }
    /* fragmented file (moof) is rejected */
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "moof");
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(io.writes == 0);
        free(b.p);
        free(io.op);
    }
    /* compressed moov (cmov) is rejected */
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "mdat");
        b32(&b, 0);
        bend(&b, x);
        x = bbox(&b, "moov");
        {
            size_t y = bbox(&b, "cmov");
            b32(&b, 0);
            bend(&b, y);
        }
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(io.writes == 0);
        free(b.p);
        free(io.op);
    }
    /* external data reference (url without self-contained flag) is rejected */
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "mdat");
        b32(&b, 0);
        bend(&b, x);
        x = bbox(&b, "moov");
        {
            size_t y = bbox(&b, "trak");
            size_t z = bbox(&b, "mdia");
            size_t w = bbox(&b, "minf");
            size_t u = bbox(&b, "dinf");
            size_t q = bbox(&b, "dref");
            b32(&b, 0); b32(&b, 1);
            {
                size_t e = bbox(&b, "url ");
                b32(&b, 0);     /* flags: not self-contained -> external reference */
                bend(&b, e);
            }
            bend(&b, q);
            bend(&b, u);
            bend(&b, w);
            bend(&b, z);
            bend(&b, y);
        }
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        TASK_CHECK(io.writes == 0);
        free(b.p);
        free(io.op);
    }
    /* duplicate moov */
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "mdat");
        b32(&b, 0);
        bend(&b, x);
        x = bbox(&b, "moov");
        bend(&b, x);
        x = bbox(&b, "moov");
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        free(b.p);
        free(io.op);
    }
    /* no moov / no mdat */
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "mdat");
        b32(&b, 0);
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        free(b.p);
        free(io.op);
    }
    {
        bld_t b = { NULL, 0, 0 };
        size_t x = bbox(&b, "ftyp");
        btag(&b, "isom"); b32(&b, 0); btag(&b, "isom"); btag(&b, "iso2");
        bend(&b, x);
        x = bbox(&b, "moov");
        bend(&b, x);
        io_init(&io); io.in.p = b.p; io.in.n = b.n;
        rc = MP4E_faststart(b.n, &io, io_read, io_write);
        TASK_CHECK(rc == MP4E_STATUS_BAD_ARGUMENTS);
        free(b.p);
        free(io.op);
    }
    return 0;
}

/* ---------------- offsets group ---------------- */

static int case_offsets_default_real(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    io_init(&io);
    io.in = in;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    /* every sample: same payload bytes, same timestamp and duration */
    {
        mem_t out = { io.op, io.on };
        TASK_CHECK(samples_match(&in, &out));
    }
    free(in.p);
    free(io.op);
    return 0;
}

static int case_offsets_sequential_real(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    TASK_CHECK(load_file("vectors/out_sequential_ref.mp4", &in));
    io_init(&io);
    io.in = in;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    /* ~300 mdats merged behind one moov: per-sample check via MP4D */
    {
        mem_t out = { io.op, io.on };
        TASK_CHECK(samples_match(&in, &out));
    }
    free(in.p);
    free(io.op);
    return 0;
}

static int case_offsets_synthetic_multi_mdat(void)
{
    synth_t s;
    io_t io;
    box_t ob[8];
    tab_t tabs[4];
    int nt = 0;
    uint64_t p1, p2;    /* output mdat payload positions */
    uint64_t d1, d2;    /* per-mdat shift of the chunk offsets */
    build_synthetic(&s);
    io_init(&io);
    io.in = s.file;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    TASK_CHECK(read_boxes(io.op, io.on, ob, 8) == 5);
    p1 = ob[2].pos + ob[2].hdr;
    p2 = ob[4].pos + ob[4].hdr;
    walk_tabs(io.op, ob[1].pos + ob[1].hdr, ob[1].pos + ob[1].size, 0, tabs, &nt, 4);
    TASK_CHECK(nt == 2);
    /* each chunk is mapped onto the new position of its own mdat payload */
    TASK_CHECK(tabs[0].ent[0] == p1 + (s.c10 - s.mdat1_payload));
    TASK_CHECK(tabs[0].ent[1] == p2 + (s.c11 - s.mdat2_payload));
    TASK_CHECK(tabs[1].ent[0] == p1 + (s.c20 - s.mdat1_payload));
    /* the two mdats shift by different amounts: no single constant delta */
    d1 = tabs[0].ent[0] - s.c10;
    d2 = tabs[0].ent[1] - s.c11;
    TASK_CHECK(d1 != d2);
    /* chunk payloads are byte-identical at the new positions */
    TASK_CHECK(!memcmp(io.op + tabs[0].ent[0], s.file.p + s.c10, 40));
    TASK_CHECK(!memcmp(io.op + tabs[0].ent[1], s.file.p + s.c11, 30));
    TASK_CHECK(!memcmp(io.op + tabs[1].ent[0], s.file.p + s.c20, 30));
    /* offsets point at payload, never at a box header */
    TASK_CHECK(tabs[0].ent[0] >= p1 && tabs[0].ent[0] < ob[2].pos + ob[2].size);
    TASK_CHECK(tabs[0].ent[1] >= p2 && tabs[0].ent[1] < ob[4].pos + ob[4].size);
    free(s.file.p);
    free(io.op);
    return 0;
}

/* ---------------- stream group ---------------- */

static int case_stream_bounded_chunks(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    box_t *ob;
    int no;
    TASK_CHECK(load_file("vectors/out_sequential_ref.mp4", &in));
    io_init(&io);
    io.in = in;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_OK);
    /* write offsets form one contiguous stream starting at 0 */
    TASK_CHECK(io.contiguous);
    TASK_CHECK(io.writes > 0 && io.reads > 0);
    /* every single read and write of payload is bounded to 64 KiB */
    TASK_CHECK(io.max_write <= 64 * 1024);
    TASK_CHECK(io.max_read <= 64 * 1024);
    TASK_CHECK(io.max_write > 0);
    /* total written equals the parsed output size */
    ob = (box_t *)malloc(1024 * sizeof(box_t));
    TASK_CHECK(ob != NULL);
    no = read_boxes(io.op, io.on, ob, 1024);
    TASK_CHECK(no > 3);
    TASK_CHECK(io.on == ob[no - 1].pos + ob[no - 1].size);
    free(ob);
    free(in.p);
    free(io.op);
    return 0;
}

static int case_stream_read_failure(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    /* read fails immediately */
    io_init(&io);
    io.in = in;
    io.fail_read_after = 0;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io.read_failed);
    TASK_CHECK(io.writes == 0);
    free(io.op);
    /* read fails while the media data is being copied out */
    io_init(&io);
    io.in = in;
    io.fail_read_after = 7;     /* past scan + moov read, inside mdat copy */
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io.read_failed);
    free(io.op);
    free(in.p);
    return 0;
}

static int case_stream_write_failure(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    /* the very first write fails */
    io_init(&io);
    io.in = in;
    io.fail_at = 0;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io.write_failed);
    TASK_CHECK(!io.write_after_fail);   /* stream stops at once */
    free(io.op);
    /* failure in the middle of the moov */
    io_init(&io);
    io.in = in;
    io.fail_at = 100;
    TASK_CHECK(run_fs(&io) == MP4E_STATUS_FILE_WRITE_ERROR);
    TASK_CHECK(io.write_failed);
    TASK_CHECK(!io.write_after_fail);
    free(io.op);
    free(in.p);
    return 0;
}

static int case_stream_bad_arguments(void)
{
    mem_t in = { NULL, 0 };
    io_t io;
    TASK_CHECK(load_file("vectors/out_ref.mp4", &in));
    io_init(&io);
    io.in = in;
    TASK_CHECK(MP4E_faststart(in.n, &io, NULL, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(MP4E_faststart(in.n, &io, io_read, NULL) == MP4E_STATUS_BAD_ARGUMENTS);
    /* input size beyond INT64_MAX cannot be addressed by the callbacks */
    TASK_CHECK(MP4E_faststart(0x8000000000000000ull, &io, io_read, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    /* empty input has no ftyp/moov/mdat */
    TASK_CHECK(MP4E_faststart(0, &io, io_read, io_write) == MP4E_STATUS_BAD_ARGUMENTS);
    TASK_CHECK(io.writes == 0);
    free(in.p);
    free(io.op);
    return 0;
}

/* ---------------- case table ---------------- */

const struct task_case task_cases[] = {
    { "layout",  "default_real",          case_layout_default_real },
    { "layout",  "sequential_real",       case_layout_sequential_real },
    { "layout",  "synthetic_multi_mdat",  case_layout_synthetic_multi_mdat },
    { "layout",  "already_faststart",     case_layout_already_faststart },
    { "layout",  "cascade_64bit",         case_layout_cascade_64bit },
    { "layout",  "reject_malformed",      case_layout_reject_malformed },
    { "offsets", "default_real",          case_offsets_default_real },
    { "offsets", "sequential_real",       case_offsets_sequential_real },
    { "offsets", "synthetic_multi_mdat",  case_offsets_synthetic_multi_mdat },
    { "stream",  "bounded_chunks",        case_stream_bounded_chunks },
    { "stream",  "read_failure",          case_stream_read_failure },
    { "stream",  "write_failure",         case_stream_write_failure },
    { "stream",  "bad_arguments",         case_stream_bad_arguments },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
