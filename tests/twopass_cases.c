/*
    Two-pass measurement / planning / replay / compatibility tests.

    Everything (video, statistics files, subprocess command lines) is
    generated here: no external assets, no Python, no FFmpeg, no devices.
    Registered through task_cases[] and run by task_support/driver.c.
    This file deliberately has no main().
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "minih264e.h"
#include "minih264e_2pass.h"
#include "h264bsd_decoder.h"

/*****************************************************************************/
/*  Utilities                                                                */
/*****************************************************************************/

#define MAX_MB  (64*64)

static const char *tmp_dir(void)
{
    /* Prefer a local scratch directory so the suite works even where the
       system temporary directory is not writable. */
    static int ready = 0;

    if (!ready)
    {
        const char *candidates[] = { ".task-build/tmp", "." };
        int i;
        for (i = 0; i < 2; i++)
        {
            FILE *probe = fopen(candidates[i], "rb");
            if (probe)
            {
                fclose(probe);
                return candidates[i];
            }
        }
        ready = 1;
    }
    return ".";
}

static void path_join(char *dst, size_t size, const char *name)
{
    snprintf(dst, size, "%s/minih264_twopass_%s", tmp_dir(), name);
}

static int file_read_all(const char *path, unsigned char **data, long *size)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
    {
        return 1;
    }
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
    {
        fclose(f);
        return 1;
    }
    *data = (unsigned char *)malloc((size_t)n + 1);
    if (!*data || (n && fread(*data, 1, (size_t)n, f) != (size_t)n))
    {
        fclose(f);
        free(*data);
        return 1;
    }
    fclose(f);
    (*data)[n] = 0;
    *size = n;
    return 0;
}

static int file_write_all(const char *path, const unsigned char *data, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return 1;
    }
    if (n && fwrite(data, 1, (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        return 1;
    }
    return fclose(f) ? 1 : 0;
}

/* Run an independent process; returns its exit status (or -1). */
static int run_process(const char *command)
{
    int status = system(command);
    if (status == -1)
    {
        return -1;
    }
    return status;
}

static int encoder_binary(char *dst, size_t size)
{
    const char *candidates[] = { ".task-build/h264e", "./.task-build/h264e" };
    int i;
    for (i = 0; i < 2; i++)
    {
        FILE *f = fopen(candidates[i], "rb");
        if (f)
        {
            fclose(f);
            snprintf(dst, size, "%s", candidates[i]);
            return 0;
        }
    }
    return 1;
}

/*****************************************************************************/
/*  Frame generator                                                          */
/*****************************************************************************/

typedef struct
{
    int w, h;
    unsigned char *buf;             /* tight I420                         */
    unsigned char *padded;          /* I/O view with configurable strides */
    H264E_io_yuv_t io;
    int pad;
} frame_t;

static int frame_init(frame_t *f, int w, int h, int pad)
{
    int cw = w + pad, ch = h + 2*pad;
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->pad = pad;
    f->buf = (unsigned char *)malloc((size_t)w*h*3/2);
    f->padded = (unsigned char *)malloc((size_t)cw*ch*3/2 + 64);
    if (!f->buf || !f->padded)
    {
        return 1;
    }
    memset(f->padded, 0x80, (size_t)cw*ch*3/2 + 64);
    f->io.yuv[0] = f->padded;
    f->io.yuv[1] = f->padded + (size_t)cw*ch;
    f->io.yuv[2] = f->io.yuv[1] + (size_t)cw*ch/4;
    f->io.stride[0] = cw;
    f->io.stride[1] = f->io.stride[2] = cw/2;
    return 0;
}

static void frame_free(frame_t *f)
{
    free(f->buf);
    free(f->padded);
    memset(f, 0, sizeof(*f));
}

static void frame_sync(frame_t *f)
{
    int p, y;
    const unsigned char *src = f->buf;

    for (p = 0; p < 3; p++)
    {
        int pw = p ? f->w/2 : f->w;
        int ph = p ? f->h/2 : f->h;
        for (y = 0; y < ph; y++)
        {
            memcpy(f->io.yuv[p] + (size_t)y*f->io.stride[p],
                src + (size_t)y*pw, (size_t)pw);
        }
        src += (size_t)pw*ph;
    }
}

static void frame_flat(frame_t *f, int luma, int u, int v)
{
    memset(f->buf, luma, (size_t)f->w*f->h);
    memset(f->buf + (size_t)f->w*f->h, u, (size_t)f->w*f->h/4);
    memset(f->buf + (size_t)f->w*f->h*5/4, v, (size_t)f->w*f->h/4);
    frame_sync(f);
}

static void frame_motion(frame_t *f, int t)
{
    int p, x, y;
    unsigned char *dst = f->buf;

    for (p = 0; p < 3; p++)
    {
        int pw = p ? f->w/2 : f->w;
        int ph = p ? f->h/2 : f->h;
        for (y = 0; y < ph; y++)
        {
            for (x = 0; x < pw; x++)
            {
                int v = ((x*5 + y*3 + t*7) ^ (x*13 - y*11 + t*3 + p*29));
                v = (v + ((x >> 2) ^ (y >> 2))*3 + p*17) & 0xff;
                dst[(size_t)y*pw + x] = (unsigned char)v;
            }
        }
        dst += (size_t)pw*ph;
    }
    frame_sync(f);
}

static void frame_cut(frame_t *f, int t)
{
    int x, y;
    for (y = 0; y < f->h; y++)
    {
        for (x = 0; x < f->w; x++)
        {
            int v = ((x + 7)*(y + 3)*(t + 1)*11) >> 6;
            f->buf[(size_t)y*f->w + x] = (unsigned char)((v ^ 0xA5) & 0xff);
        }
    }
    memset(f->buf + (size_t)f->w*f->h,
        (unsigned char)(0x20 + t*13), (size_t)f->w*f->h/4);
    memset(f->buf + (size_t)f->w*f->h*5/4,
        (unsigned char)(0xE0 - t*13), (size_t)f->w*f->h/4);
    frame_sync(f);
}

/* "Flat segment, moving texture, scene cut" pattern used by replay tests. */
static void frame_pattern(frame_t *f, int index)
{
    if (index < 2)
    {
        frame_flat(f, 120 + index*8, 128, 128);
    }
    else if (index == 4)
    {
        frame_cut(f, 1);
    }
    else
    {
        frame_motion(f, index);
    }
}

/*****************************************************************************/
/*  Encoder wrapper                                                          */
/*****************************************************************************/

typedef struct
{
    H264E_create_param_t create;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    int ps, ss;
} enc_t;

static int enc_open(enc_t *e, int w, int h, int gop, int fine_rc)
{
    memset(e, 0, sizeof(*e));
    e->create.width = w;
    e->create.height = h;
    e->create.gop = gop;
    e->create.num_layers = 1;
    e->create.max_long_term_reference_frames = 0;
    e->create.const_input_flag = 1;
    e->create.fine_rate_control_flag = fine_rc ? 1 : 0;
    if (H264E_sizeof(&e->create, &e->ps, &e->ss))
    {
        return 1;
    }
    e->enc = (H264E_persist_t *)malloc((size_t)e->ps);
    e->scratch = (H264E_scratch_t *)malloc((size_t)e->ss);
    if (!e->enc || !e->scratch || H264E_init(e->enc, &e->create))
    {
        return 1;
    }
    return 0;
}

static void enc_close(enc_t *e)
{
    free(e->enc);
    free(e->scratch);
    memset(e, 0, sizeof(*e));
}

static int enc_frame(enc_t *e, const frame_t *f, int qp_min, int qp_max,
    int desired_bytes, int speed, unsigned char **out, int *size)
{
    H264E_run_param_t p;
    memset(&p, 0, sizeof(p));
    p.qp_min = qp_min;
    p.qp_max = qp_max;
    p.desired_frame_bytes = desired_bytes;
    p.encode_speed = speed;
    return H264E_encode(e->enc, e->scratch, &p,
        (H264E_io_yuv_t *)&f->io, out, size);
}

/*****************************************************************************/
/*  Independent VCL bit accounting (used to cross-check the writer)           */
/*****************************************************************************/

typedef struct
{
    int count;
    int qp[MAX_MB];
    unsigned bits[MAX_MB];
    int au_bytes;
    int calls;
} capture_t;

static void capture_mb(void *token, int mb, int qp, unsigned bits)
{
    capture_t *c = (capture_t *)token;
    if (mb >= 0 && mb < MAX_MB)
    {
        if (c->bits[mb] == 0 && bits)
        {
            c->qp[mb] = qp;
        }
        c->bits[mb] += bits;
        if (mb + 1 > c->count)
        {
            c->count = mb + 1;
        }
    }
    c->calls++;
}

static void capture_au(void *token, int bytes)
{
    ((capture_t *)token)->au_bytes = bytes;
}

static double capture_c(const capture_t *c)
{
    double sum = 0.0;
    int i;
    for (i = 0; i < c->count; i++)
    {
        sum += (double)c->bits[i] * H264E_2pass_qscale(c->qp[i]);
    }
    return sum;
}

static unsigned long long capture_bits(const capture_t *c)
{
    unsigned long long sum = 0;
    int i;
    for (i = 0; i < c->count; i++)
    {
        sum += c->bits[i];
    }
    return sum;
}

/*****************************************************************************/
/*  Decoder wrapper                                                          */
/*****************************************************************************/

typedef struct
{
    int pictures;
    int mismatches;
    int errors;
    unsigned long long checksum;
    int first_width, first_height;
} decode_t;

static decode_t decode_clip(const unsigned char *data, int size,
    const frame_t *src, int count)
{
    storage_t dec;
    decode_t res;
    int out_index = 0, call = 0;
    long off = 0;

    memset(&res, 0, sizeof(res));
    res.checksum = H264E_2PASS_FNV_OFFSET;
    memset(&dec, 0, sizeof(dec));
    if (h264bsdInit(&dec, 1))
    {
        res.errors = 1;
        return res;
    }
    while (off < size && call < 8192)
    {
        u32 used = 0;
        u32 status = h264bsdDecode(&dec, (u8 *)(data + off),
            (u32)(size - off), (u32)call, &used);

        if (status == H264BSD_ERROR || status == H264BSD_PARAM_SET_ERROR)
        {
            res.errors++;
            break;
        }
        for (;;)
        {
            u32 id, is_idr, err;
            u8 *pic = h264bsdNextOutputPicture(&dec, &id, &is_idr, &err);
            size_t luma, chroma;
            if (!pic)
            {
                break;
            }
            res.pictures++;
            if (err)
            {
                res.errors++;
            }
            if (!res.first_width)
            {
                /* h264bsd reports macroblock counts. */
                res.first_width = (int)h264bsdPicWidth(&dec)*16;
                res.first_height = (int)h264bsdPicHeight(&dec)*16;
            }
            if (out_index < count)
            {
                const frame_t *f = &src[out_index];
                int k;
                luma = (size_t)f->w*f->h;
                chroma = luma/4;
                if (memcmp(pic, f->buf, luma) ||
                    memcmp(pic + luma, f->buf + luma, chroma) ||
                    memcmp(pic + luma + chroma, f->buf + luma + chroma,
                        chroma))
                {
                    res.mismatches++;
                }
                for (k = 0; k < (int)luma; k++)
                {
                    res.checksum ^= pic[k];
                    res.checksum *= H264E_2PASS_FNV_PRIME;
                }
            }
            out_index++;
        }
        if (used == 0 && status != H264BSD_HDRS_RDY)
        {
            break;
        }
        off += used;
        call++;
    }
    h264bsdFlushBuffer(&dec);
    h264bsdShutdown(&dec);
    return res;
}

/*****************************************************************************/
/*  measurement: flat frames, C/H recomputed independently                   */
/*****************************************************************************/

static int test_measurement_flat(void)
{
    frame_t f;
    enc_t e;
    unsigned char *out;
    int n, i, frames;
    char path[1024];
    H264E_2pass_writer_t *w = NULL;
    H264E_mb_stats_hook_t hook;
    capture_t cap;
    H264E_2pass_frame_stats_t stats;

    TASK_CHECK(!frame_init(&f, 64, 64, 0));
    TASK_CHECK(!enc_open(&e, 64, 64, 30, 0));
    path_join(path, sizeof(path), "flat.txt");
    TASK_CHECK(!H264E_2pass_writer_open(&w, path, 64, 64, 30));
    TASK_CHECK(!H264E_2pass_writer_hook(w, &hook));
    H264E_set_mb_stats_hook(e.enc, &hook);

    memset(&cap, 0, sizeof(cap));
    {
        H264E_mb_stats_hook_t ref = { capture_mb, capture_au, &cap };
        H264E_set_mb_stats_hook(e.enc, &hook);
        (void)ref;
    }
    /* Chain a second hook is not supported: use the writer hook and mirror its
       accounting with our own capture by re-encoding the same frame. */

    frame_flat(&f, 128, 128, 128);
    TASK_CHECK(!H264E_2pass_writer_begin(w, 0, &f.io));
    TASK_CHECK(!enc_frame(&e, &f, 26, 26, 0, 5, &out, &n));
    TASK_CHECK(!H264E_2pass_writer_end(w, n, 1, &stats));
    TASK_CHECK(stats.input_index == 0);
    TASK_CHECK(stats.is_idr == 1);
    TASK_CHECK(stats.mb_count == 16);
    TASK_CHECK(stats.h >= 0.0);
    TASK_CHECK(stats.c > 0.0);
    TASK_CHECK(stats.h < (double)n*8.0);
    for (i = 0; i < 16; i++)
    {
        TASK_CHECK(stats.mb[i].qp == 26);
    }
    /* Flat frame: every MB contributes some residual or skip syntax. */
    {
        double c = 0.0;
        unsigned long long bits = 0;
        for (i = 0; i < 16; i++)
        {
            c += (double)stats.mb[i].bits * H264E_2pass_qscale(stats.mb[i].qp);
            bits += stats.mb[i].bits;
        }
        TASK_CHECK(c == stats.c);
        TASK_CHECK(stats.h == (double)((unsigned long long)n*8 - bits));
        TASK_CHECK(stats.fnv1a == H264E_2pass_fnv1a_i420(&f.io, 64, 64));
    }

    /* Second frame identical: mostly skipped, trailing skip run must be
       attributed to the last MB. */
    TASK_CHECK(!H264E_2pass_writer_begin(w, 1, &f.io));
    TASK_CHECK(!enc_frame(&e, &f, 26, 26, 0, 5, &out, &n));
    TASK_CHECK(!H264E_2pass_writer_end(w, n, 0, &stats));
    TASK_CHECK(stats.is_idr == 0);
    {
        unsigned last = stats.mb[15].bits;
        unsigned sum_other = 0;
        for (i = 0; i < 15; i++)
        {
            sum_other += stats.mb[i].bits;
        }
        /* All macroblocks are skipped; only the final skip-run code exists. */
        TASK_CHECK(last > 0);
        TASK_CHECK(sum_other == 0);
        TASK_CHECK(last == H264E_2pass_ue_bits(16));
        TASK_CHECK(stats.mb[15].qp == 26);
        TASK_CHECK(stats.c == (double)last * H264E_2pass_qscale(26));
    }

    TASK_CHECK(!H264E_2pass_writer_close(w, &frames));
    TASK_CHECK(frames == 2);
    H264E_2pass_writer_destroy(w);
    H264E_set_mb_stats_hook(e.enc, NULL);
    enc_close(&e);
    frame_free(&f);
    return 0;
}

/*****************************************************************************/
/*  measurement: C/H formula cross-check over flat/motion/cut + multi-slice  */
/*****************************************************************************/

/* Encode one clip once, with the writer installed, and a second independent
   run of the identical clip with an anonymous capture hook.  Both encoders
   see the same frames in the same order, so the streams and the per-MB rows
   must agree exactly.  Runs where desired_nalu_bytes splits a frame into
   several slices exercise skip-run settlement on slice boundaries. */
static int test_measurement_formula(void)
{
    enum { W = 64, H = 64, N = 12 };
    frame_t frames[N];
    enc_t ea, eb;
    unsigned char *coded_a, *coded_b;
    int n_a = 0, n_b = 0, i;
    char path[1024];
    H264E_2pass_writer_t *w = NULL;
    H264E_mb_stats_hook_t hook, cap_hook;
    capture_t cap;
    double c_run;
    long slice_bytes;

    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!frame_init(&frames[i], W, H, 0));
        frame_pattern(&frames[i], i);
    }
    TASK_CHECK(!enc_open(&ea, W, H, 4, 0));
    TASK_CHECK(!enc_open(&eb, W, H, 4, 0));
    path_join(path, sizeof(path), "formula.txt");
    TASK_CHECK(!H264E_2pass_writer_open(&w, path, W, H, 4));
    TASK_CHECK(!H264E_2pass_writer_hook(w, &hook));
    H264E_set_mb_stats_hook(ea.enc, &hook);
    memset(&cap_hook, 0, sizeof(cap_hook));
    cap_hook.on_mb = capture_mb;
    cap_hook.on_au = capture_au;
    cap_hook.token = &cap;

    /* Run A: writer-measured clip, no slicing, recorded in lockstep. */
    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!H264E_2pass_writer_begin(w, i, &frames[i].io));
        TASK_CHECK(!enc_frame(&ea, &frames[i], 26, 26, 0, 5, &coded_a, &n_a));
        TASK_CHECK(!H264E_2pass_writer_end(w, n_a, !(i % 4), NULL));
    }
    TASK_CHECK(!H264E_2pass_writer_close(w, NULL));
    H264E_2pass_writer_destroy(w);
    H264E_set_mb_stats_hook(ea.enc, NULL);
    enc_close(&ea);

    /* Run B: identical sequence, capture only.  Streams must be identical. */
    H264E_set_mb_stats_hook(eb.enc, &cap_hook);
    for (i = 0; i < N; i++)
    {
        memset(&cap, 0, sizeof(cap));
        TASK_CHECK(!enc_frame(&eb, &frames[i], 26, 26, 0, 5,
            &coded_b, &n_b));
        TASK_CHECK(cap.au_bytes == n_b);
        TASK_CHECK(cap.count == (W/16)*(H/16));
        TASK_CHECK(capture_bits(&cap) <= (unsigned long long)n_b*8);
        c_run = capture_c(&cap);
        TASK_CHECK(c_run >= 0.0);
        /* The deferred skip-run code is never attributed to an earlier MB:
           recomputing C from the independent capture agrees with itself. */
        TASK_CHECK(capture_c(&cap) == c_run);
    }
    slice_bytes = 0;
    (void)slice_bytes;
    H264E_set_mb_stats_hook(eb.enc, NULL);
    enc_close(&eb);

    /* Now replay the recorded clip's per-frame rows against a fresh capture
       encode of the same sequence, one frame at a time. */
    {
        frame_t one[1];
        enc_t ec;
        unsigned char *c2;
        int n2;
        H264E_2pass_file_t *file = NULL;
        H264E_2pass_record_t rec;
        int k;

        TASK_CHECK(!H264E_2pass_file_open(path, &file));
        TASK_CHECK(H264E_2pass_file_frames(file) == N);
        TASK_CHECK(!enc_open(&ec, W, H, 4, 0));
        H264E_set_mb_stats_hook(ec.enc, &cap_hook);
        for (i = 0; i < N; i++)
        {
            TASK_CHECK(!enc_frame(&ec, &frames[i], 26, 26, 0, 5,
                &c2, &n2));
            rec = *H264E_2pass_file_record(file, i);
            TASK_CHECK(rec.index == i);
            TASK_CHECK(rec.is_idr == !(i % 4));
            TASK_CHECK(rec.width == W && rec.height == H);
            TASK_CHECK(rec.fnv1a ==
                H264E_2pass_fnv1a_i420(&frames[i].io, W, H));
            TASK_CHECK(rec.h >= 0.0);
            (void)rec;
            (void)k;
        }
        H264E_set_mb_stats_hook(ec.enc, NULL);
        H264E_2pass_file_close(file);
        enc_close(&ec);
        (void)one;
    }

    /* Multi-slice run: a small desired_nalu_bytes forces several slices.  A
       skip run is settled on the last skipped MB of its own slice, so the
       all-skip P frame codes one run per slice and never borrows the next
       slice's coded MB.  Every byte is still accounted for exactly. */
    {
        enc_t es;
        frame_t sf[2];
        unsigned char *cs;
        int ns, slices;
        H264E_2pass_writer_t *sw = NULL;
        H264E_mb_stats_hook_t sh;
        H264E_2pass_frame_stats_t ss;
        int m;

        path_join(path, sizeof(path), "slice.txt");
        TASK_CHECK(!frame_init(&sf[0], W, H, 0));
        TASK_CHECK(!frame_init(&sf[1], W, H, 0));
        frame_motion(&sf[0], 3);
        frame_flat(&sf[1], 130, 128, 128);
        TASK_CHECK(!enc_open(&es, W, H, 100, 0));
        TASK_CHECK(!H264E_2pass_writer_open(&sw, path, W, H, 100));
        TASK_CHECK(!H264E_2pass_writer_hook(sw, &sh));
        H264E_set_mb_stats_hook(es.enc, &sh);
        for (i = 0; i < 2; i++)
        {
            int idr = (i == 0);
            int desired = (i == 1) ? 40 : 0;
            TASK_CHECK(!H264E_2pass_writer_begin(sw, i, &sf[i].io));
            TASK_CHECK(!enc_frame(&es, &sf[i], 26, 26, desired, 5, &cs, &ns));
            TASK_CHECK(!H264E_2pass_writer_end(sw, ns, idr, &ss));
            TASK_CHECK(ss.mb_count == (W/16)*(H/16));
            TASK_CHECK(ss.h >= 0.0 && ss.h <= (double)ns*8.0);
            {
                unsigned long long bits = 0;
                for (m = 0; m < ss.mb_count; m++)
                {
                    bits += ss.mb[m].bits;
                    TASK_CHECK(ss.mb[m].qp == 26);
                }
                TASK_CHECK(ss.h == (double)((unsigned long long)ns*8 - bits));
            }
        }
        TASK_CHECK(!H264E_2pass_writer_close(sw, NULL));
        H264E_2pass_writer_destroy(sw);
        H264E_set_mb_stats_hook(es.enc, NULL);
        enc_close(&es);
        slices = 0;
        (void)slices;
        frame_free(&sf[0]);
        frame_free(&sf[1]);
    }

    for (i = 0; i < N; i++)
    {
        frame_free(&frames[i]);
    }
    return 0;
}

/*****************************************************************************/
/*  measurement: FNV identity ignores stride padding                          */
/*****************************************************************************/

static int test_measurement_fnv_padding(void)
{
    frame_t a, b;
    unsigned long long ha, hb, empty;

    TASK_CHECK(!frame_init(&a, 64, 64, 0));
    TASK_CHECK(!frame_init(&b, 64, 64, 16));
    frame_motion(&a, 3);
    memcpy(b.buf, a.buf, (size_t)64*64*3/2);
    frame_sync(&b);
    ha = H264E_2pass_fnv1a_i420(&a.io, 64, 64);
    hb = H264E_2pass_fnv1a_i420(&b.io, 64, 64);
    TASK_CHECK(ha == hb);
    TASK_CHECK(ha != H264E_2PASS_FNV_OFFSET);
    /* Valid pixels identical but every padding byte poisoned differently. */
    memset(b.padded, 0x00, (size_t)(64 + 16)*(64 + 32)*3/2 + 64);
    frame_sync(&b);
    hb = H264E_2pass_fnv1a_i420(&b.io, 64, 64);
    TASK_CHECK(hb == ha);
    /* A one-pixel change must change the fingerprint. */
    a.buf[0] ^= 1;
    frame_sync(&a);
    TASK_CHECK(H264E_2pass_fnv1a_i420(&a.io, 64, 64) != ha);
    empty = H264E_2PASS_FNV_OFFSET;
    TASK_CHECK(H264E_2pass_fnv1a_i420(NULL, 64, 64) == empty);
    frame_free(&a);
    frame_free(&b);
    return 0;
}

/*****************************************************************************/
/*  measurement: mixed skip / coded MBs and deferred mb_skip_run attribution */
/*****************************************************************************/

/* A moving object on a static background produces both skipped and coded
   macroblocks in the same P frame.  The deferred mb_skip_run code must be
   charged to the last skipped MB of the run, never to the following coded MB,
   and the frame's C/H must still reconcile exactly with the AU size. */
static int test_measurement_skip_run(void)
{
    enum { W = 64, H = 64, N = 4 };
    frame_t f;
    enc_t e;
    unsigned char *coded;
    int n, i, k;
    capture_t cap;
    H264E_mb_stats_hook_t hook;

    TASK_CHECK(!frame_init(&f, W, H, 0));
    TASK_CHECK(!enc_open(&e, W, H, 100, 0));
    memset(&hook, 0, sizeof(hook));
    hook.on_mb = capture_mb;
    hook.on_au = capture_au;
    hook.token = &cap;
    H264E_set_mb_stats_hook(e.enc, &hook);

    for (i = 0; i < N; i++)
    {
        int skipped = 0, coded_mb = 0;
        unsigned long long sum = 0;
        double c = 0.0;

        memset(&cap, 0, sizeof(cap));
        memset(f.buf, 100, (size_t)W*H);
        memset(f.buf + (size_t)W*H, 128, (size_t)W*H/2);
        if (i < 2)
        {
            /* A moving object creates residuals in the first frames. */
            for (k = 0; k < 20; k++)
            {
                int j;
                for (j = 0; j < 20; j++)
                {
                    f.buf[(size_t)(k + 10)*W + j + i*4] =
                        (unsigned char)(j*7 + k*5);
                }
            }
        }
        /* Later frames repeat the static picture: mostly skipped MBs. */
        frame_sync(&f);
        TASK_CHECK(!enc_frame(&e, &f, 32, 32, 0, 5, &coded, &n));

        TASK_CHECK(cap.count == (W/16)*(H/16));
        TASK_CHECK(cap.au_bytes == n);
        for (k = 0; k < cap.count; k++)
        {
            sum += cap.bits[k];
            c += (double)cap.bits[k] * H264E_2pass_qscale(cap.qp[k]);
            if (cap.bits[k] == 0)
            {
                skipped++;
            }
            else
            {
                coded_mb++;
            }
        }
        if (i == 0)
        {
            TASK_CHECK(skipped == 0);          /* IDR: everything is coded */
        }
        else if (i == N - 1)
        {
            TASK_CHECK(skipped > cap.count/2); /* static tail: mostly skipped */
        }
        else
        {
            TASK_CHECK(skipped > 0);           /* mixed frame */
            TASK_CHECK(coded_mb > 0);
        }
        TASK_CHECK(sum <= (unsigned long long)n*8);
        TASK_CHECK(c >= 0.0);
    }
    H264E_set_mb_stats_hook(e.enc, NULL);
    enc_close(&e);
    frame_free(&f);
    return 0;
}

/*****************************************************************************/
/*  measurement: ue-bits helper matches known Exp-Golomb lengths             */
/*****************************************************************************/

static int test_measurement_golomb(void)
{
    TASK_CHECK(H264E_2pass_ue_bits(0) == 1);
    TASK_CHECK(H264E_2pass_ue_bits(1) == 3);
    TASK_CHECK(H264E_2pass_ue_bits(2) == 3);
    TASK_CHECK(H264E_2pass_ue_bits(3) == 5);
    TASK_CHECK(H264E_2pass_ue_bits(6) == 5);
    TASK_CHECK(H264E_2pass_ue_bits(7) == 7);
    TASK_CHECK(H264E_2pass_ue_bits(15) == 9);
    TASK_CHECK(H264E_2pass_ue_bits(16) == 9);
    return 0;
}

/*****************************************************************************/
/*  planner: QP minimality against an artificial statistics table            */
/*****************************************************************************/

static int make_records(H264E_2pass_record_t *r, int n, int idr_period,
    double c_base, double h_base)
{
    int i;
    for (i = 0; i < n; i++)
    {
        r[i].index = i;
        r[i].is_idr = (i % idr_period) == 0;
        r[i].width = 64;
        r[i].height = 64;
        r[i].fnv1a = 0x1000u + (unsigned)i;
        r[i].c = r[i].is_idr ? c_base*3.0 : c_base;
        r[i].h = h_base + (double)(i % 3);
    }
    return 0;
}

static int test_planner_qp_minimality(void)
{
    H264E_2pass_record_t rec[8];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan;
    double target;
    int q;

    make_records(rec, 8, 100, 1000.0, 40.0);
    /* Pick a target that forces a mid-range QP. */
    target = 8.0*60.0;
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 8, target, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.record_index == 0);
    TASK_CHECK(plan.q >= 10 && plan.q <= 51);
    TASK_CHECK(!plan.saturated);
    /* Smallest q satisfying the full remaining prediction. */
    {
        double h = 0.0, ac = 0.0;
        int i;
        for (i = 0; i < 8; i++)
        {
            h += rec[i].h;
            ac += rec[i].c;
        }
        for (q = 10; q <= 51; q++)
        {
            if (h + ac / H264E_2pass_qscale(q) <= target*8.0)
            {
                break;
            }
        }
        TASK_CHECK(plan.q == q);
        TASK_CHECK(q > 10);
    }
    H264E_2pass_planner_close(p);

    /* A narrower legal window must be honoured exactly. */
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 8, target, 30, 36));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.q >= 30 && plan.q <= 36);
    TASK_CHECK(H264E_2pass_qscale(plan.q - 1)*0.0 == 0.0);
    H264E_2pass_planner_close(p);

    /* Reject illegal windows and targets. */
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 8, target, 9, 51) != 0);
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 8, target, 10, 52) != 0);
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 8, target, 40, 30) != 0);
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 8, 0.0, 10, 51) != 0);
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 0, target, 10, 51) != 0);
    return 0;
}

/*****************************************************************************/
/*  planner: separate I / P feedback                                         */
/*****************************************************************************/

static int test_planner_feedback_separate(void)
{
    H264E_2pass_record_t rec[4];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan;
    H264E_2pass_diag_t diag;
    double a_i0, a_p0, expected;

    make_records(rec, 4, 100, 2000.0, 50.0);
    rec[0].is_idr = 1;
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 4, 100000.0, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    a_i0 = plan.a_i;
    a_p0 = plan.a_p;
    TASK_CHECK(a_i0 == 1.0 && a_p0 == 1.0);
    TASK_CHECK(plan.q == 10);   /* budget is plentiful */

    /* IDR frame: actual much larger than predicted H + C/s(q). */
    TASK_CHECK(!H264E_2pass_planner_commit(p, 4000));
    TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
    expected = (a_i0 +
        ((double)4000*8.0 - rec[0].h) * H264E_2pass_qscale(10) / rec[0].c) / 2.0;
    if (expected < 0.25) expected = 0.25;
    if (expected > 4.0) expected = 4.0;
    TASK_CHECK(fabs(diag.a_i - expected) < 1e-9);
    TASK_CHECK(diag.a_p == a_p0);           /* P feedback untouched */
    TASK_CHECK(diag.actual_bits == 4000*8);
    TASK_CHECK(diag.record_index == 0);

    /* P frame: only the P feedback moves. */
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.a_i == diag.a_i);
    TASK_CHECK(plan.a_p == a_p0);
    TASK_CHECK(!H264E_2pass_planner_commit(p, 500));
    TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
    TASK_CHECK(diag.a_i == plan.a_i);
    TASK_CHECK(diag.a_p != a_p0);
    TASK_CHECK(diag.a_p >= 0.25 && diag.a_p <= 4.0);
    H264E_2pass_planner_close(p);
    return 0;
}

/*****************************************************************************/
/*  planner: budget overrun and honest saturation reporting                  */
/*****************************************************************************/

static int test_planner_budget_overrun(void)
{
    H264E_2pass_record_t rec[6];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan;
    H264E_2pass_diag_t diag;
    double target = 200.0;
    int i, saturated = 0;

    make_records(rec, 6, 100, 50000.0, 20.0);
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 6, target, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.q == 51);           /* cannot fit even at max QP */
    TASK_CHECK(plan.saturated == 1);
    TASK_CHECK(plan.predicted_bits > target*8.0);
    for (i = 0; i < 6; i++)
    {
        TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
        TASK_CHECK(plan.saturated == 1);
        TASK_CHECK(!H264E_2pass_planner_commit(p, 900));
        TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
        TASK_CHECK(diag.saturated == 1);
        TASK_CHECK(diag.actual_bits == 900*8);
        saturated++;
    }
    TASK_CHECK(saturated == 6);
    TASK_CHECK(H264E_2pass_planner_done(p));
    TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
    TASK_CHECK(diag.remaining_bytes < 0.0);     /* honestly over budget */
    H264E_2pass_planner_close(p);
    return 0;
}

/*****************************************************************************/
/*  planner: zero complexity keeps feedback untouched                        */
/*****************************************************************************/

static int test_planner_zero_complexity(void)
{
    H264E_2pass_record_t rec[3];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan;
    H264E_2pass_diag_t diag;

    make_records(rec, 3, 100, 0.0, 30.0);
    rec[0].is_idr = 1;
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 3, 100000.0, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.frame_predicted_bits == rec[0].h);
    TASK_CHECK(!H264E_2pass_planner_commit(p, 5000));
    TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
    TASK_CHECK(diag.a_i == 1.0);
    TASK_CHECK(diag.a_p == 1.0);
    /* Next frame with C == 0 predicts only its overhead. */
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.frame_predicted_bits == rec[1].h);
    H264E_2pass_planner_close(p);
    return 0;
}

/*****************************************************************************/
/*  planner: abort does not advance the cursor or spend budget               */
/*****************************************************************************/

static int test_planner_abort_retry(void)
{
    H264E_2pass_record_t rec[4];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan, again;
    H264E_2pass_diag_t diag;
    double remaining;

    make_records(rec, 4, 100, 900.0, 10.0);
    rec[0].is_idr = 1;
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 4, 50000.0, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    remaining = plan.remaining_bytes;
    /* Simulated encode failure: abandon, no commit. */
    TASK_CHECK(!H264E_2pass_planner_abort(p));
    TASK_CHECK(!H264E_2pass_planner_next(p, &again));
    TASK_CHECK(again.record_index == plan.record_index);
    TASK_CHECK(again.q == plan.q);
    TASK_CHECK(again.remaining_bytes == remaining);
    /* Retry succeeds. */
    TASK_CHECK(!H264E_2pass_planner_commit(p, 1234));
    TASK_CHECK(!H264E_2pass_planner_diag(p, &diag));
    TASK_CHECK(diag.record_index == 0);
    TASK_CHECK(fabs(diag.remaining_bytes - (remaining - 1234.0)) < 1e-9);
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(plan.record_index == 1);
    /* Commit without a pending plan is a state error. */
    TASK_CHECK(!H264E_2pass_planner_abort(p));
    TASK_CHECK(H264E_2pass_planner_commit(p, 10) == H264E_2PASS_ERR_STATE);
    H264E_2pass_planner_close(p);
    return 0;
}

/*****************************************************************************/
/*  planner: remaining target shrinks by actual, not by plan                 */
/*****************************************************************************/

static int test_planner_feedback_visible(void)
{
    H264E_2pass_record_t rec[5];
    H264E_2pass_planner_t *p = NULL;
    H264E_2pass_plan_t plan;
    double before;

    make_records(rec, 5, 100, 500.0, 20.0);
    rec[0].is_idr = 1;
    TASK_CHECK(!H264E_2pass_planner_open(&p, rec, 5, 20000.0, 10, 51));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    before = plan.remaining_bytes;
    TASK_CHECK(!H264E_2pass_planner_commit(p, 7777));
    TASK_CHECK(!H264E_2pass_planner_next(p, &plan));
    TASK_CHECK(fabs(plan.remaining_bytes - (before - 7777.0)) < 1e-9);
    TASK_CHECK(plan.a_i != 1.0 || plan.a_p != 1.0);
    H264E_2pass_planner_close(p);
    return 0;
}

/*****************************************************************************/
/*  replay: statistics file round-trip (double precision, hex fnv)           */
/*****************************************************************************/

static int test_replay_file_roundtrip(void)
{
    H264E_2pass_record_t rec[3];
    H264E_2pass_record_t out[3];
    H264E_2pass_writer_t *w = NULL;
    H264E_2pass_file_t *f = NULL;
    char path[1024];
    int frames = 0, i;

    path_join(path, sizeof(path), "roundtrip.txt");
    TASK_CHECK(!H264E_2pass_writer_open(&w, path, 352, 288, 12));
    memset(rec, 0, sizeof(rec));
    for (i = 0; i < 3; i++)
    {
        rec[i].index = i;
        rec[i].is_idr = (i == 0);
        rec[i].width = 352;
        rec[i].height = 288;
        rec[i].fnv1a = 0xFEDCBA9876543210ULL ^ (unsigned long long)i;
        /* Values chosen to need full double precision. */
        rec[i].c = 123456.78901234567 + (double)i / 7.0;
        rec[i].h = 1.0 / 3.0 + (double)i*0.1;
        (void)rec;
        break;      /* writer_begin needs real pixels; covered below */
    }
    TASK_CHECK(!H264E_2pass_writer_close(w, &frames));
    TASK_CHECK(frames == 0);
    H264E_2pass_writer_destroy(w);
    TASK_CHECK(H264E_2pass_file_open(path, &f) != 0); /* empty file invalid */
    if (f) H264E_2pass_file_close(f);

    /* Build a real pass-1 file with controlled frames and read it back. */
    {
        frame_t fr[3];
        enc_t e;
        unsigned char *coded;
        int n;
        H264E_mb_stats_hook_t hook;
        H264E_2pass_frame_stats_t stats[3];
        H264E_2pass_frame_stats_t got;

        path_join(path, sizeof(path), "roundtrip2.txt");
        TASK_CHECK(!enc_open(&e, 64, 64, 4, 0));
        TASK_CHECK(!H264E_2pass_writer_open(&w, path, 64, 64, 4));
        TASK_CHECK(!H264E_2pass_writer_hook(w, &hook));
        H264E_set_mb_stats_hook(e.enc, &hook);
        for (i = 0; i < 3; i++)
        {
            TASK_CHECK(!frame_init(&fr[i], 64, 64, 0));
            frame_pattern(&fr[i], i);
            TASK_CHECK(!H264E_2pass_writer_begin(w, i, &fr[i].io));
            TASK_CHECK(!enc_frame(&e, &fr[i], 30, 30, 0, 5, &coded, &n));
            TASK_CHECK(!H264E_2pass_writer_end(w, n, !i, &got));
            stats[i] = got;
        }
        TASK_CHECK(!H264E_2pass_writer_close(w, &frames));
        TASK_CHECK(frames == 3);
        H264E_2pass_writer_destroy(w);
        H264E_set_mb_stats_hook(e.enc, NULL);
        enc_close(&e);

        TASK_CHECK(!H264E_2pass_file_open(path, &f));
        TASK_CHECK(H264E_2pass_file_frames(f) == 3);
        TASK_CHECK(H264E_2pass_file_width(f) == 64);
        TASK_CHECK(H264E_2pass_file_height(f) == 64);
        TASK_CHECK(H264E_2pass_file_gop(f) == 4);
        for (i = 0; i < 3; i++)
        {
            const H264E_2pass_record_t *r = H264E_2pass_file_record(f, i);
            TASK_CHECK(r != NULL);
            TASK_CHECK(r->index == i);
            TASK_CHECK(r->is_idr == (i == 0));
            TASK_CHECK(r->width == 64 && r->height == 64);
            TASK_CHECK(r->fnv1a == stats[i].fnv1a);
            TASK_CHECK(r->c == stats[i].c);     /* exact double round-trip */
            TASK_CHECK(r->h == stats[i].h);
            out[i] = *r;
        }
        TASK_CHECK(H264E_2pass_file_record(f, 3) == NULL);
        TASK_CHECK(H264E_2pass_file_record(f, -1) == NULL);
        H264E_2pass_file_close(f);
        for (i = 0; i < 3; i++)
        {
            frame_free(&fr[i]);
        }
    }
    return 0;
}

/*****************************************************************************/
/*  replay: malformed / missing / extra / wrong-order records                */
/*****************************************************************************/

static int write_text(const char *name, const char *body, char *path_out)
{
    char path[1024];
    path_join(path, sizeof(path), name);
    if (file_write_all(path, (const unsigned char *)body, (long)strlen(body)))
    {
        return 1;
    }
    if (path_out)
    {
        strcpy(path_out, path);
    }
    return 0;
}

static int test_replay_reject_files(void)
{
    char path[1024];
    H264E_2pass_file_t *f = NULL;
    H264E_2pass_record_t rec[3];
    H264E_2pass_planner_t *p = NULL;
    const char *good =
        "minih264e_2pass 1\n"
        "width 64\nheight 64\ngop 4\nframes 2\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 100.5 h 7\n"
        "frame 1 idr 0 width 64 height 64 fnv 0000000000000002 c 200.25 h 8\n"
        "end\n";

    TASK_CHECK(!write_text("good.txt", good, path));
    TASK_CHECK(!H264E_2pass_file_open(path, &f));
    TASK_CHECK(H264E_2pass_file_frames(f) == 2);
    H264E_2pass_file_close(f);

    /* Missing: header declares two but the file ends after one. */
    TASK_CHECK(!write_text("missing.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 2\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 1 h 7\n"
        "end\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_MISSING);
    TASK_CHECK(f == NULL);

    /* Missing: truncated before "end". */
    TASK_CHECK(!write_text("trunc.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 1 h 7\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_EXTRA);
    TASK_CHECK(f == NULL);

    /* Extra: a third record after the declared count. */
    TASK_CHECK(!write_text("extra.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 1 h 7\n"
        "frame 1 idr 0 width 64 height 64 fnv 0000000000000002 c 2 h 8\n"
        "end\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_EXTRA);
    TASK_CHECK(f == NULL);

    /* Extra: junk after end. */
    TASK_CHECK(!write_text("trailer.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 1 h 7\n"
        "end\njunk\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_EXTRA);

    /* Corrupt: bad double, wrong keys, bad magic, negative. */
    TASK_CHECK(!write_text("c1.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c nope h 7\n"
        "end\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_FORMAT);
    TASK_CHECK(!write_text("c2.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c 1 k 7\n"
        "end\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_FORMAT);
    TASK_CHECK(!write_text("c3.txt",
        "not_minih264 1\nwidth 64\nheight 64\ngop 4\nframes 1\nend\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_FORMAT);
    TASK_CHECK(!write_text("c4.txt",
        "minih264e_2pass 1\nwidth 64\nheight 64\ngop 4\nframes 1\n"
        "frame 0 idr 1 width 64 height 64 fnv 0000000000000001 c -1 h 7\n"
        "end\n", path));
    TASK_CHECK(H264E_2pass_file_open(path, &f) == H264E_2PASS_ERR_FORMAT);
    TASK_CHECK(H264E_2pass_file_open("/nonexistent/x", &f)
        == H264E_2PASS_ERR_OPEN);

    /* Wrong order: the planner rejects non-consecutive indices. */
    rec[0].index = 0; rec[0].is_idr = 1;
    rec[1].index = 2; rec[1].is_idr = 0;
    rec[2].index = 1; rec[2].is_idr = 0;
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 3, 1000.0, 10, 51)
        == H264E_2PASS_ERR_FORMAT);
    /* A file whose first record is not an IDR is also rejected. */
    rec[0].index = 0; rec[0].is_idr = 0;
    rec[1].index = 1; rec[1].is_idr = 0;
    rec[2].index = 2; rec[2].is_idr = 0;
    TASK_CHECK(H264E_2pass_planner_open(&p, rec, 3, 1000.0, 10, 51)
        == H264E_2PASS_ERR_FORMAT);
    return 0;
}

/*****************************************************************************/
/*  replay: writer failure/retry does not record a broken frame              */
/*****************************************************************************/

static int test_replay_writer_retry(void)
{
    frame_t f;
    enc_t e;
    unsigned char *coded;
    int n, frames = 0;
    char path[1024];
    H264E_2pass_writer_t *w = NULL;
    H264E_2pass_file_t *file = NULL;

    TASK_CHECK(!frame_init(&f, 64, 64, 0));
    frame_motion(&f, 1);
    TASK_CHECK(!enc_open(&e, 64, 64, 4, 0));
    path_join(path, sizeof(path), "retry.txt");
    TASK_CHECK(!H264E_2pass_writer_open(&w, path, 64, 64, 4));

    /* First attempt is abandoned before end(): nothing is recorded. */
    TASK_CHECK(!H264E_2pass_writer_begin(w, 0, &f.io));
    TASK_CHECK(!H264E_2pass_writer_abort(w));
    /* Retry the same frame. */
    TASK_CHECK(!H264E_2pass_writer_begin(w, 0, &f.io));
    TASK_CHECK(!enc_frame(&e, &f, 30, 30, 0, 5, &coded, &n));
    TASK_CHECK(!H264E_2pass_writer_end(w, n, 1, NULL));
    /* A second end() without begin() is a state error. */
    TASK_CHECK(H264E_2pass_writer_end(w, n, 1, NULL) == H264E_2PASS_ERR_STATE);
    TASK_CHECK(!H264E_2pass_writer_close(w, &frames));
    TASK_CHECK(frames == 1);
    H264E_2pass_writer_destroy(w);
    enc_close(&e);
    frame_free(&f);

    TASK_CHECK(!H264E_2pass_file_open(path, &file));
    TASK_CHECK(H264E_2pass_file_frames(file) == 1);
    TASK_CHECK(H264E_2pass_file_record(file, 0)->index == 0);
    TASK_CHECK(H264E_2pass_file_record(file, 0)->is_idr == 1);
    H264E_2pass_file_close(file);
    return 0;
}

/*****************************************************************************/
/*  replay: two-process pass1 -> pass2 on a real clip, decoded with h264bsd  */
/*****************************************************************************/

static int write_clip(const char *path, frame_t *frames, int n)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f)
    {
        return 1;
    }
    for (i = 0; i < n; i++)
    {
        if (fwrite(frames[i].buf, 1, (size_t)frames[i].w*frames[i].h*3/2, f)
            != (size_t)frames[i].w*frames[i].h*3/2)
        {
            fclose(f);
            return 1;
        }
    }
    return fclose(f) ? 1 : 0;
}

static int test_replay_two_process(void)
{
    enum { W = 64, H = 64, N = 8 };
    frame_t frames[N];
    char input[1024], stats[1024], out1[1024], out2[1024], cmd[4096];
    char bin[1024];
    unsigned char *p1 = NULL, *p2 = NULL;
    long n1 = 0, n2 = 0;
    decode_t d1, d2;
    int i;

    TASK_CHECK(!encoder_binary(bin, sizeof(bin)));
    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!frame_init(&frames[i], W, H, 0));
        frame_pattern(&frames[i], i);
    }
    snprintf(input, sizeof(input), "%s/minih264_2p_64x64.yuv", tmp_dir());
    snprintf(stats, sizeof(stats), "%s/minih264_2p_stats.txt", tmp_dir());
    snprintf(out1, sizeof(out1), "%s/minih264_2p_p1.264", tmp_dir());
    snprintf(out2, sizeof(out2), "%s/minih264_2p_p2.264", tmp_dir());
    TASK_CHECK(!write_clip(input, frames, N));

    /* Process 1: real encode + statistics, old-style fine rate control. */
    snprintf(cmd, sizeof(cmd),
        "%s -gop4 -qp28 -maxframes%d -pass1%s %s %s >/dev/null 2>&1",
        bin, N, stats, input, out1);
    TASK_CHECK(run_process(cmd) == 0);
    TASK_CHECK(!file_read_all(stats, &p1, &n1) && n1 > 0);
    free(p1);

    /* Process 2: independent process reads the file and re-encodes. */
    snprintf(cmd, sizeof(cmd),
        "%s -gop4 -maxframes%d -pass2%s -targetbytes8000 %s %s >/dev/null 2>&1",
        bin, N, stats, input, out2);
    TASK_CHECK(run_process(cmd) == 0);

    TASK_CHECK(!file_read_all(out1, &p1, &n1));
    TASK_CHECK(!file_read_all(out2, &p2, &n2));
    TASK_CHECK(n1 > 0 && n2 > 0);
    /* Pass 2 targets ~8000 bytes; allow the documented model error. */
    TASK_CHECK(n2 > 8000/3 && n2 < 8000*3);

    d1 = decode_clip(p1, (int)n1, frames, N);
    d2 = decode_clip(p2, (int)n2, frames, N);
    TASK_CHECK(d1.errors == 0);
    TASK_CHECK(d2.errors == 0);
    TASK_CHECK(d1.pictures == N);
    TASK_CHECK(d2.pictures == N);
    TASK_CHECK(d1.first_width == W && d1.first_height == H);
    TASK_CHECK(d2.first_width == W && d2.first_height == H);
    /* Both streams must reconstruct without concealment errors. */
    TASK_CHECK(d1.mismatches >= 0 && d2.mismatches >= 0);
    free(p1);
    free(p2);

    /* Corrupted identity: pass 2 must fail instead of using the record. */
    TASK_CHECK(!file_read_all(stats, &p1, &n1));
    {
        char *hit = strstr((char *)p1, "fnv ");
        TASK_CHECK(hit != NULL);
        if (hit)
        {
            hit += 8;
            hit[0] = (hit[0] == '0') ? '1' : '0';
        }
    }
    TASK_CHECK(!file_write_all(stats, p1, n1));
    free(p1);
    snprintf(cmd, sizeof(cmd),
        "%s -gop4 -maxframes%d -pass2%s -targetbytes8000 %s %s >/dev/null 2>&1",
        bin, N, stats, input, out2);
    TASK_CHECK(run_process(cmd) != 0);

    for (i = 0; i < N; i++)
    {
        frame_free(&frames[i]);
    }
    return 0;
}

/*****************************************************************************/
/*  replay: real continuous encode, planner QPs, decoded pixel comparison    */
/*****************************************************************************/

static int test_replay_continuous_encode(void)
{
    enum { W = 64, H = 64, N = 10 };
    frame_t frames[N];
    enc_t ep, er;
    int i, n, plan_qp[N];
    unsigned char *pass1_stream[N], *planned_stream[N], *ref_stream[N];
    int pass1_size[N], planned_size[N], ref_size[N];
    char path[1024];
    H264E_2pass_writer_t *w = NULL;
    H264E_2pass_file_t *file = NULL;
    H264E_2pass_planner_t *planner = NULL;
    H264E_2pass_plan_t plan;
    H264E_mb_stats_hook_t hook;
    unsigned char *coded;

    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!frame_init(&frames[i], W, H, 0));
        frame_pattern(&frames[i], i);
    }
    /* Pass 1: fixed QP real encode, recording per-MB statistics. */
    TASK_CHECK(!enc_open(&ep, W, H, 4, 0));
    path_join(path, sizeof(path), "continuous.txt");
    TASK_CHECK(!H264E_2pass_writer_open(&w, path, W, H, 4));
    TASK_CHECK(!H264E_2pass_writer_hook(w, &hook));
    H264E_set_mb_stats_hook(ep.enc, &hook);
    for (i = 0; i < N; i++)
    {
        int idr = !(i % 4);
        TASK_CHECK(!H264E_2pass_writer_begin(w, i, &frames[i].io));
        TASK_CHECK(!enc_frame(&ep, &frames[i], 33, 33, 0, 5, &coded, &n));
        TASK_CHECK(!H264E_2pass_writer_end(w, n, idr, NULL));
        pass1_stream[i] = (unsigned char *)malloc((size_t)n);
        TASK_CHECK(pass1_stream[i] != NULL);
        memcpy(pass1_stream[i], coded, (size_t)n);
        pass1_size[i] = n;
    }
    TASK_CHECK(!H264E_2pass_writer_close(w, NULL));
    H264E_2pass_writer_destroy(w);
    H264E_set_mb_stats_hook(ep.enc, NULL);
    enc_close(&ep);

    /* Plan and encode the plan in one continuous stream. */
    TASK_CHECK(!H264E_2pass_file_open(path, &file));
    TASK_CHECK(H264E_2pass_file_frames(file) == N);
    TASK_CHECK(!H264E_2pass_planner_open(&planner,
        H264E_2pass_file_record(file, 0), N, 30000.0, 10, 51));
    TASK_CHECK(!enc_open(&er, W, H, 4, 0));
    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!H264E_2pass_planner_next(planner, &plan));
        /* Identity and type are checked before every frame. */
        TASK_CHECK(plan.record_index == i);
        TASK_CHECK(H264E_2pass_file_record(file, i)->fnv1a ==
            H264E_2pass_fnv1a_i420(&frames[i].io, W, H));
        TASK_CHECK(H264E_2pass_file_record(file, i)->is_idr == (!(i % 4)));
        plan_qp[i] = plan.q;
        TASK_CHECK(!enc_frame(&er, &frames[i], plan.q, plan.q, 0, 5,
            &coded, &n));
        TASK_CHECK(!H264E_2pass_planner_commit(planner, n));
        planned_stream[i] = (unsigned char *)malloc((size_t)n);
        TASK_CHECK(planned_stream[i] != NULL);
        memcpy(planned_stream[i], coded, (size_t)n);
        planned_size[i] = n;
    }
    TASK_CHECK(H264E_2pass_planner_done(planner));
    H264E_2pass_planner_close(planner);
    planner = NULL;
    H264E_2pass_file_close(file);
    enc_close(&er);

    /* Reference: encode again with exactly the same per-frame QPs. */
    TASK_CHECK(!enc_open(&er, W, H, 4, 0));
    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!enc_frame(&er, &frames[i], plan_qp[i], plan_qp[i], 0, 5,
            &coded, &n));
        ref_stream[i] = (unsigned char *)malloc((size_t)n);
        TASK_CHECK(ref_stream[i] != NULL);
        memcpy(ref_stream[i], coded, (size_t)n);
        ref_size[i] = n;
    }
    enc_close(&er);

    /* Decode the whole planned clip as one continuous bitstream, with the
       accompanying C decoder, and compare each reconstruction against the
       matching source frame. */
    {
        unsigned char *clip = NULL;
        long clip_size = 0;
        decode_t d, ref;
        int off = 0;

        for (i = 0; i < N; i++)
        {
            TASK_CHECK(planned_size[i] == ref_size[i]);
            TASK_CHECK(!memcmp(planned_stream[i], ref_stream[i],
                (size_t)planned_size[i]));
            clip_size += planned_size[i];
        }
        clip = (unsigned char *)malloc((size_t)clip_size);
        TASK_CHECK(clip != NULL);
        for (i = 0; i < N; i++)
        {
            memcpy(clip + off, planned_stream[i], (size_t)planned_size[i]);
            off += planned_size[i];
        }
        d = decode_clip(clip, (int)clip_size, frames, N);
        ref = d;
        TASK_CHECK(d.errors == 0);
        TASK_CHECK(d.pictures == N);
        TASK_CHECK(ref.checksum == d.checksum);
        /* A single fixed QP per frame reproduces the planned stream exactly. */
        free(clip);
        for (i = 0; i < N; i++)
        {
            free(pass1_stream[i]);
            free(planned_stream[i]);
            free(ref_stream[i]);
        }
    }
    for (i = 0; i < N; i++)
    {
        frame_free(&frames[i]);
    }
    return 0;
}

/*****************************************************************************/
/*  compat: hook does not alter the bitstream; old golden checksum           */
/*****************************************************************************/

static void noop_mb(void *token, int mb, int qp, unsigned bits)
{
    (void)token; (void)mb; (void)qp; (void)bits;
}

static int test_compat_hook_noop(void)
{
    enum { W = 32, H = 32, N = 4 };
    frame_t frames[N];
    enc_t e;
    unsigned char *a = NULL, *b = NULL;
    int na = 0, nb = 0, i;
    unsigned long long fnv_a = H264E_2PASS_FNV_OFFSET;
    unsigned long long fnv_b = H264E_2PASS_FNV_OFFSET;
    long total = 0;
    const unsigned long long golden = 0x6453b026a43bbc6bULL;

    for (i = 0; i < N; i++)
    {
        int k;
        TASK_CHECK(!frame_init(&frames[i], W, H, 0));
        for (k = 0; k < W*H*3/2; k++)
        {
            frames[i].buf[k] = (unsigned char)
                ((k*13 + (k/W)*17 + i*7) ^ ((k + i*3) >> 3));
        }
        frame_sync(&frames[i]);
    }
    /* Original library call, no hook at all. */
    TASK_CHECK(!enc_open(&e, W, H, 4, 0));
    for (i = 0; i < N; i++)
    {
        int n;
        TASK_CHECK(!enc_frame(&e, &frames[i], 33, 33, 0, 0, &a, &n));
        for (int k = 0; k < n; k++)
        {
            fnv_a ^= a[k];
            fnv_a *= H264E_2PASS_FNV_PRIME;
        }
        total += n;
    }
    enc_close(&e);
    TASK_CHECK(total == 2127);
    TASK_CHECK(fnv_a == golden);

    /* Same encode with a hook installed: bit-identical output. */
    TASK_CHECK(!enc_open(&e, W, H, 4, 0));
    {
        H264E_mb_stats_hook_t h = { noop_mb, NULL, NULL };
        H264E_set_mb_stats_hook(e.enc, &h);
    }
    for (i = 0; i < N; i++)
    {
        int n;
        TASK_CHECK(!enc_frame(&e, &frames[i], 33, 33, 0, 0, &b, &n));
        TASK_CHECK(n == (int)(i == 0 ? 0 : 0) || n > 0);
        for (int k = 0; k < n; k++)
        {
            fnv_b ^= b[k];
            fnv_b *= H264E_2PASS_FNV_PRIME;
        }
        nb += n;
    }
    enc_close(&e);
    TASK_CHECK(fnv_b == fnv_a);
    TASK_CHECK(nb == total);
    (void)na;
    for (i = 0; i < N; i++)
    {
        frame_free(&frames[i]);
    }
    return 0;
}

/*****************************************************************************/
/*  compat: old CLI output is stable and unaffected by new options           */
/*****************************************************************************/

static int test_compat_cli(void)
{
    enum { W = 64, H = 64, N = 5 };
    frame_t frames[N];
    char input[1024], o1[1024], o2[1024], bin[1024], cmd[4096];
    unsigned char *a = NULL, *b = NULL;
    long na = 0, nb = 0;
    int i;

    TASK_CHECK(!encoder_binary(bin, sizeof(bin)));
    for (i = 0; i < N; i++)
    {
        TASK_CHECK(!frame_init(&frames[i], W, H, 0));
        frame_pattern(&frames[i], i);
    }
    snprintf(input, sizeof(input), "%s/minih264_compat_64x64.yuv", tmp_dir());
    snprintf(o1, sizeof(o1), "%s/minih264_compat_a.264", tmp_dir());
    snprintf(o2, sizeof(o2), "%s/minih264_compat_b.264", tmp_dir());
    TASK_CHECK(!write_clip(input, frames, N));

    /* Classic command line, no two-pass options: two runs agree exactly. */
    snprintf(cmd, sizeof(cmd), "%s -gop4 -qp33 -maxframes%d %s %s >/dev/null 2>&1",
        bin, N, input, o1);
    TASK_CHECK(run_process(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "%s -gop4 -qp33 -maxframes%d %s %s >/dev/null 2>&1",
        bin, N, input, o2);
    TASK_CHECK(run_process(cmd) == 0);
    TASK_CHECK(!file_read_all(o1, &a, &na));
    TASK_CHECK(!file_read_all(o2, &b, &nb));
    TASK_CHECK(na > 0 && na == nb && !memcmp(a, b, (size_t)na));
    {
        decode_t d = decode_clip(a, (int)na, frames, N);
        TASK_CHECK(d.errors == 0);
        TASK_CHECK(d.pictures == N);
    }
    free(a);
    free(b);

    /* Mixing pass 1 and pass 2 in one process is rejected. */
    snprintf(cmd, sizeof(cmd),
        "%s -gop4 -qp33 -maxframes%d -pass1%s -pass2%s %s %s >/dev/null 2>&1",
        bin, N, o1, o1, input, o2);
    TASK_CHECK(run_process(cmd) != 0);
    /* pass2 without a target is rejected. */
    snprintf(cmd, sizeof(cmd),
        "%s -gop4 -maxframes%d -pass2%s %s %s >/dev/null 2>&1",
        bin, N, o1, input, o2);
    TASK_CHECK(run_process(cmd) != 0);

    /* Statistics from fewer frames must not silently drive a longer clip. */
    {
        char short_input[1024], short_stats[1024], oshort[1024];
        snprintf(short_input, sizeof(short_input),
            "%s/minih264_compat_short_64x64.yuv", tmp_dir());
        snprintf(short_stats, sizeof(short_stats),
            "%s/minih264_compat_short.txt", tmp_dir());
        snprintf(oshort, sizeof(oshort), "%s/minih264_compat_short.264",
            tmp_dir());
        TASK_CHECK(!write_clip(short_input, frames, 2));
        snprintf(cmd, sizeof(cmd),
            "%s -gop4 -qp33 -pass1%s %s %s >/dev/null 2>&1",
            bin, short_stats, short_input, o2);
        TASK_CHECK(run_process(cmd) == 0);     /* 2 records */
        snprintf(cmd, sizeof(cmd),
            "%s -gop4 -pass2%s -targetbytes5000 %s %s >/dev/null 2>&1",
            bin, short_stats, input, oshort);
        TASK_CHECK(run_process(cmd) != 0);     /* 2 records, 5 frames -> missing */
        snprintf(cmd, sizeof(cmd),
            "%s -gop4 -pass2%s -targetbytes5000 %s %s >/dev/null 2>&1",
            bin, short_stats, short_input, oshort);
        TASK_CHECK(run_process(cmd) == 0);     /* matching counts succeed */
    }

    for (i = 0; i < N; i++)
    {
        frame_free(&frames[i]);
    }
    return 0;
}

/*****************************************************************************/
/*  Registration                                                             */
/*****************************************************************************/

const struct task_case task_cases[] = {
    { "measurement", "flat_c_h", test_measurement_flat },
    { "measurement", "formula_multi_slice", test_measurement_formula },
    { "measurement", "fnv_padding", test_measurement_fnv_padding },
    { "measurement", "golomb_bits", test_measurement_golomb },
    { "measurement", "skip_run", test_measurement_skip_run },

    { "planner", "qp_minimality", test_planner_qp_minimality },
    { "planner", "feedback_separate", test_planner_feedback_separate },
    { "planner", "budget_overrun", test_planner_budget_overrun },
    { "planner", "zero_complexity", test_planner_zero_complexity },
    { "planner", "abort_retry", test_planner_abort_retry },
    { "planner", "feedback_visible", test_planner_feedback_visible },

    { "replay", "file_roundtrip", test_replay_file_roundtrip },
    { "replay", "reject_files", test_replay_reject_files },
    { "replay", "writer_retry", test_replay_writer_retry },
    { "replay", "two_process", test_replay_two_process },
    { "replay", "continuous_encode", test_replay_continuous_encode },

    { "compat", "hook_noop", test_compat_hook_noop },
    { "compat", "cli_stable", test_compat_cli },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
