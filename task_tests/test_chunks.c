/**************************************************************************************
 * Regression tests for the chunked (low-latency streaming) bitstream output.
 *
 *   prefix    - callback timing vs. row mode search + actual prefix bytes
 *   coding    - whole-sequence bitstream/reconstruction equivalence and
 *               adversarial coder/bit-writer states vs. the batch path
 *   lifecycle - final-chunk discipline, interleaved instances, NULL callback
 **************************************************************************************/

#include "task_support/check.h"

#include "uavs3e.h"
#include "define.h"
#include "modules.h"
#include "task_src/enc_chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_FRAMES 16

/*-------------------------------------------------------------------------*/
/* common helpers                                                          */
/*-------------------------------------------------------------------------*/

static void stream_cfg(enc_cfg_t *c, int w, int h)
{
    uavs3e_load_default_cfg(c);
    c->horizontal_size   = w;
    c->vertical_size     = h;
    c->fps_num           = 25;
    c->fps_den           = 1;
    c->bit_depth_input   = 8;
    c->bit_depth_internal= 8;
    c->ctu_size          = 64;
    c->max_b_frames      = 0;
    c->frm_threads       = 1;
    c->wpp_threads       = 1;
    c->adaptive_gop      = 0;
    c->lookahead         = 0;
    c->i_period          = 16;
    c->speed_level       = 0;
    c->qp                = 34;
    c->rc_type           = RC_TYPE_NULL;
    c->adaptive_dqp      = 0;
    c->alf_enable        = 0;
    c->sao_enable        = 0;
    c->use_pic_sign      = 0;
    c->use_deblock       = 1;
}

static void fill_frame(com_img_t *img, int f)
{
    for (int k = 0; k < 3; k++) {
        for (int y = 0; y < img->height[k]; y++) {
            unsigned char *p = (unsigned char *)img->planes[k] + y * img->stride[k];
            for (int x = 0; x < img->width[k]; x++) {
                p[x] = (unsigned char)((x * 13 + y * 7 + (x * y) % 37 + f * 11 + k * 61) & 255);
            }
        }
    }
}

static uint64_t fnv1a(uint64_t h, const unsigned char *p, int n)
{
    while (n--) {
        h = (h ^ *p++) * 1099511628211ULL;
    }
    return h;
}

static uint64_t rec_hash(com_img_t *rec)
{
    uint64_t h = 1469598103934665603ULL;
    for (int k = 0; k < 3; k++) {
        for (int y = 0; y < rec->height[k]; y++) {
            h = fnv1a(h, (unsigned char *)rec->planes[k] + y * rec->stride[k], rec->width[k]);
        }
    }
    return h;
}

/*-------------------------------------------------------------------------*/
/* chunk sink: reassembles the callback chunks of one picture              */
/*-------------------------------------------------------------------------*/

typedef struct {
    unsigned char asm_buf[512 * 1024];
    int  asm_len;
    int  calls;
    int  final_calls;
    int  prev_completed;     /* completed_rows of the previous chunk, -1 at frame start */
    int  monotonic_ok;
    int  frame_final_seen;
    int  after_final;        /* chunks delivered after the final one of a frame */
    int  final_rows;
} sink_t;

static void sink_init(sink_t *sk)
{
    memset(sk, 0, sizeof(*sk));
    sk->monotonic_ok = 1;
    sk->prev_completed = -1;
}

static void sink_frame_begin(sink_t *sk)
{
    sk->asm_len          = 0;
    sk->prev_completed   = -1;
    sk->frame_final_seen = 0;
}

static void cb_sink(void *opaque, long long poc, const unsigned char *data, int bytes, int completed_rows, int final)
{
    sink_t *sk = (sink_t *)opaque;
    (void)poc;
    if (sk->frame_final_seen) {
        sk->after_final++;
    }
    if (sk->asm_len + bytes > (int)sizeof(sk->asm_buf)) {
        sk->monotonic_ok = 0; /* overflow: fail loudly via checks */
        return;
    }
    memcpy(sk->asm_buf + sk->asm_len, data, bytes);
    sk->asm_len += bytes;
    sk->calls++;
    if (completed_rows < sk->prev_completed) {
        sk->monotonic_ok = 0;
    }
    sk->prev_completed = completed_rows;
    if (final) {
        sk->final_calls++;
        sk->final_rows       = completed_rows;
        sk->frame_final_seen = 1;
        sk->prev_completed   = -1;
    }
}

/*-------------------------------------------------------------------------*/
/* prefix group: event log of row-search entries vs. chunk callbacks       */
/*-------------------------------------------------------------------------*/

enum { EV_ROW = 1, EV_CHUNK = 2 };

typedef struct {
    int       type;
    long long poc;
    int       lcu_y;
    int       bytes;
    int       completed;
    int       final;
    int       off;          /* offset of this chunk in the reassembled stream */
} ev_t;

static ev_t  g_ev[1024];
static int   g_ev_n;
static unsigned char g_asm[512 * 1024];
static int   g_asm_len;

static void hook_row(long long poc, int lcu_y)
{
    if (g_ev_n < (int)(sizeof(g_ev) / sizeof(g_ev[0]))) {
        g_ev[g_ev_n].type      = EV_ROW;
        g_ev[g_ev_n].poc       = poc;
        g_ev[g_ev_n].lcu_y     = lcu_y;
        g_ev[g_ev_n].bytes     = 0;
        g_ev[g_ev_n].completed = 0;
        g_ev[g_ev_n].final     = 0;
        g_ev[g_ev_n].off       = 0;
        g_ev_n++;
    }
}

static void cb_prefix(void *opaque, long long poc, const unsigned char *data, int bytes, int completed_rows, int final)
{
    (void)opaque;
    if (g_ev_n < (int)(sizeof(g_ev) / sizeof(g_ev[0]))) {
        g_ev[g_ev_n].type      = EV_CHUNK;
        g_ev[g_ev_n].poc       = poc;
        g_ev[g_ev_n].lcu_y     = -1;
        g_ev[g_ev_n].bytes     = bytes;
        g_ev[g_ev_n].completed = completed_rows;
        g_ev[g_ev_n].final     = final;
        g_ev[g_ev_n].off       = g_asm_len;
        g_ev_n++;
    }
    memcpy(g_asm + g_asm_len, data, bytes);
    g_asm_len += bytes;
}

static int test_prefix_timing(void)
{
    enc_cfg_t c;
    stream_cfg(&c, 128, 256);
    c.output_chunk = cb_prefix;

    const int rows = 256 / 64; /* CTU rows per frame */

    g_ev_n    = 0;
    g_asm_len = 0;
    uavs3e_test_row_observe = hook_row;

    int err = 0;
    void *h = uavs3e_create(&c, &err);
    TASK_CHECK(h != NULL);

    for (int f = 0; f < 3 && h; f++) {
        com_img_t *img = NULL;
        TASK_CHECK(uavs3e_get_img(h, &img) == COM_OK);
        fill_frame(img, f);
        img->pts = f;

        int ev_start = g_ev_n;
        g_asm_len = 0;

        enc_stat_t s;
        memset(&s, 0, sizeof(s));
        int ret = uavs3e_enc(h, &s, img);
        TASK_CHECK(ret == COM_OK && s.bytes > 0 && s.rec_img != NULL);

        /* the reassembled chunks are exactly the returned picture bytes */
        TASK_CHECK(g_asm_len == s.bytes);
        TASK_CHECK(memcmp(g_asm, s.buf, s.bytes) == 0);

        int first_chunk_bytes = -1;
        int header_end        = -1;
        int prev_completed    = -1;
        int finals            = 0;
        int last_row_ev       = -1;
        int early_entropy     = 0;
        int delivered         = 0;

        for (int i = ev_start; i < g_ev_n; i++) {
            ev_t *e = &g_ev[i];
            TASK_CHECK(e->poc == s.poc);
            if (e->type == EV_ROW) {
                TASK_CHECK(e->lcu_y >= 0 && e->lcu_y < rows);
                if (e->lcu_y == rows - 1 && last_row_ev < 0) {
                    last_row_ev = i;
                }
                continue;
            }
            /* chunk event: its bytes are the true prefix of stat.buf at this point */
            TASK_CHECK(e->off + e->bytes <= s.bytes);
            TASK_CHECK(memcmp(g_asm + e->off, (unsigned char *)s.buf + e->off, e->bytes) == 0);
            if (first_chunk_bytes < 0) {
                first_chunk_bytes = e->bytes;
                TASK_CHECK(e->completed == 0); /* headers first, no CTU row yet */
                header_end = e->bytes;
            }
            TASK_CHECK(e->completed >= prev_completed);
            prev_completed = e->completed;
            delivered += e->bytes;
            if (e->final) {
                finals++;
            } else if (e->completed > 0 && delivered > header_end + 4 && (last_row_ev < 0 || i < last_row_ev)) {
                /* non-final chunk carrying real post-patch-header entropy
                 * bytes before the last row's mode search began */
                early_entropy = 1;
            }
        }
        TASK_CHECK(first_chunk_bytes > 0);
        TASK_CHECK(last_row_ev >= 0);          /* last row's mode search observed */
        TASK_CHECK(finals == 1);               /* exactly one final chunk */
        TASK_CHECK(g_ev[g_ev_n - 1].type == EV_CHUNK && g_ev[g_ev_n - 1].final);
        TASK_CHECK(early_entropy);
        /* the patch unit header goes out as 00 00 01 00 right after the headers */
        TASK_CHECK(header_end >= 0 && header_end + 4 <= s.bytes);
        TASK_CHECK(g_asm[header_end + 0] == 0x00 && g_asm[header_end + 1] == 0x00 &&
                   g_asm[header_end + 2] == 0x01 && g_asm[header_end + 3] == 0x00);
    }

    if (h) {
        enc_stat_t s;
        memset(&s, 0, sizeof(s));
        TASK_CHECK(uavs3e_enc(h, &s, NULL) == COM_OK_NO_MORE_FRM);
        uavs3e_free(h);
    }
    uavs3e_test_row_observe = NULL;
    return 0;
}

/*-------------------------------------------------------------------------*/
/* coding group: whole-sequence equivalence, callback on vs. off           */
/*-------------------------------------------------------------------------*/

typedef struct {
    unsigned char *buf[MAX_FRAMES];
    unsigned char *asm_buf[MAX_FRAMES];
    int      bytes[MAX_FRAMES];
    int      type[MAX_FRAMES];
    float    qp[MAX_FRAMES];
    uint64_t rec[MAX_FRAMES];
    int      n;
} enc_result_t;

static int encode_seq(enc_cfg_t *cfg, int nframes, sink_t *sink, enc_result_t *res)
{
    int err = 0;
    void *h = uavs3e_create(cfg, &err);
    if (!h) {
        return 1;
    }
    memset(res, 0, sizeof(*res));
    for (int f = 0; f < nframes; f++) {
        com_img_t *img = NULL;
        if (uavs3e_get_img(h, &img) != COM_OK) {
            return 2;
        }
        fill_frame(img, f);
        img->pts = f;
        if (sink) {
            sink_frame_begin(sink);
        }
        enc_stat_t s;
        memset(&s, 0, sizeof(s));
        if (uavs3e_enc(h, &s, img) != COM_OK || s.bytes <= 0 || !s.rec_img) {
            return 3;
        }
        int i = res->n++;
        res->buf[i]  = (unsigned char *)malloc(s.bytes);
        memcpy(res->buf[i], s.buf, s.bytes);
        res->bytes[i] = s.bytes;
        res->type[i]  = s.type;
        res->qp[i]    = s.qp;
        res->rec[i]   = rec_hash(s.rec_img);
        if (sink) {
            res->asm_buf[i] = (unsigned char *)malloc(sink->asm_len > 0 ? sink->asm_len : 1);
            memcpy(res->asm_buf[i], sink->asm_buf, sink->asm_len);
            if (sink->asm_len != s.bytes) {
                return 4;
            }
        }
    }
    enc_stat_t s;
    memset(&s, 0, sizeof(s));
    if (uavs3e_enc(h, &s, NULL) != COM_OK_NO_MORE_FRM) {
        return 5;
    }
    uavs3e_free(h);
    return 0;
}

static void free_result(enc_result_t *res)
{
    for (int i = 0; i < res->n; i++) {
        free(res->buf[i]);
        free(res->asm_buf[i]);
    }
}

static int test_coding_sequence(void)
{
    const int nframes = 6; /* one I frame followed by reference frames */

    enc_cfg_t c0, c1;
    stream_cfg(&c0, 128, 256);
    stream_cfg(&c1, 128, 256);

    sink_t sink;
    sink_init(&sink);
    c1.output_chunk        = cb_sink;
    c1.output_chunk_opaque = &sink;

    enc_result_t ref, str;
    TASK_CHECK(encode_seq(&c0, nframes, NULL, &ref) == 0);
    TASK_CHECK(encode_seq(&c1, nframes, &sink, &str) == 0);

    TASK_CHECK(ref.n == nframes && str.n == nframes);
    TASK_CHECK(ref.type[0] == 1);              /* I frame ... */
    int pframes = 0;
    for (int i = 0; i < nframes; i++) {
        /* identical whole-picture bytes with and without the callback */
        TASK_CHECK(str.bytes[i] == ref.bytes[i]);
        TASK_CHECK(str.type[i]  == ref.type[i]);
        TASK_CHECK(str.qp[i]    == ref.qp[i]);
        TASK_CHECK(memcmp(str.buf[i], ref.buf[i], ref.bytes[i]) == 0);
        /* chunks of the streaming run reassemble to the same bytes */
        TASK_CHECK(memcmp(str.asm_buf[i], ref.buf[i], ref.bytes[i]) == 0);
        /* identical reconstructed pixels */
        TASK_CHECK(str.rec[i] == ref.rec[i]);
        if (i > 0 && ref.type[i] != 1) {
            pframes++;
        }
    }
    TASK_CHECK(pframes >= 4);                  /* ... and multiple reference frames */
    TASK_CHECK(sink.final_calls == nframes);   /* one final chunk per frame */
    TASK_CHECK(sink.monotonic_ok);
    TASK_CHECK(sink.after_final == 0);

    free_result(&ref);
    free_result(&str);
    return 0;
}

/*-------------------------------------------------------------------------*/
/* coding group: constructed coder / bit-writer states, streaming vs. batch */
/*-------------------------------------------------------------------------*/

/* batch emulation-prevention of one patch unit, via the original whole-block path */
static int batch_demulate(const unsigned char *raw, int raw_len, unsigned char *out)
{
    unsigned char buf[8192];
    unsigned char tmp[8192];
    bs_t bs;
    memcpy(buf, raw, raw_len);
    bs_init(&bs, buf, tmp, (int)sizeof(buf));
    bs.cur = buf + raw_len; /* the raw bytes are already "written" */
    bs_demulate(&bs);
    int n = (int)(bs.cur - bs.start);
    memcpy(out, buf, n);
    return n;
}

/* incremental emulation-prevention of the same unit, one byte at a time */
static int stream_demulate(const unsigned char *raw, int raw_len, unsigned char *out)
{
    enc_chunk_stream_t cs;
    enc_chunk_stream_begin(&cs, out, raw[3]);
    for (int i = 4; i < raw_len; i++) {
        enc_chunk_stream_feed(&cs, raw[i]);
    }
    enc_chunk_stream_finish(&cs);
    return cs.out_len;
}

static int check_unit(const unsigned char *raw, int raw_len)
{
    unsigned char a[8192], b[8192];
    int na = batch_demulate(raw, raw_len, a);
    int nb = stream_demulate(raw, raw_len, b);
    TASK_CHECK(na == nb);
    TASK_CHECK(memcmp(a, b, na) == 0);
    return 0;
}

/* build a patch unit whose slice data is produced by a real arithmetic-coder
 * termination state: a pending byte plus a long run of stacked 0xff bytes,
 * resolved with or without carry */
static int make_lbac_unit(unsigned char *raw, int carry, unsigned pending, int nff)
{
    unsigned char buf[256];
    unsigned char tmp[256];
    bs_t bs;
    lbac_t lbac;

    bs_init(&bs, buf, tmp, (int)sizeof(buf));
    lbac_reset(&lbac);
    lbac.is_pending_byte = 1;
    lbac.pending_byte    = pending;
    lbac.stacked_ff      = (u32)nff;
    lbac.left_bits       = 23;
    lbac.code            = carry ? (1u << 9) : 0u; /* terminate with/without carry */
    lbac_finish(&lbac, &bs);
    bs_flush(&bs);

    int n = (int)(bs.cur - bs.start);
    raw[0] = 0x00; raw[1] = 0x00; raw[2] = 0x01; raw[3] = 0x00;
    memcpy(raw + 4, buf, n);
    return 4 + n;
}

static unsigned int g_rng = 0x12345678u;
static unsigned int rnd_u32(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int test_coding_demulate(void)
{
    unsigned char raw[4096];
    unsigned char a[8192], b[8192];

    /* 1) long 0xff pending run resolved by a carry: pending byte increments,
     *    the stacked 0xff bytes turn into a run of 0x00 (which then triggers
     *    emulation-prevention insertions) */
    int len = make_lbac_unit(raw, 1, 0x41, 5);
    TASK_CHECK(raw[4] == 0x42);                 /* carry propagated into pending byte */
    TASK_CHECK(raw[5] == 0x00 && raw[9] == 0x00); /* stacked 0xff became zeros */
    TASK_CHECK(check_unit(raw, len) == 0);

    /* the same state without carry: pending byte then a long 0xff run */
    len = make_lbac_unit(raw, 0, 0x41, 5);
    TASK_CHECK(raw[4] == 0x41);
    TASK_CHECK(raw[5] == 0xff && raw[9] == 0xff);
    TASK_CHECK(check_unit(raw, len) == 0);

    /* 2) consecutive zero bytes: repeated two-bit insertions, walking the
     *    insertion offset through 2/4/6/8 (byte reprocess) */
    raw[0] = 0x00; raw[1] = 0x00; raw[2] = 0x01; raw[3] = 0x00;
    memset(raw + 4, 0x00, 24);
    TASK_CHECK(check_unit(raw, 28) == 0);

    /* 3) two-bit insertion before 00 00 01/02/03 patterns mixed with data */
    {
        static const unsigned char data[] = {
            0x11, 0x00, 0x00, 0x03, 0x55, 0x00, 0x00, 0x02,
            0x00, 0x00, 0x01, 0x7f, 0x00, 0x00, 0x00, 0x00,
            0xab, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x01
        };
        raw[0] = 0x00; raw[1] = 0x00; raw[2] = 0x01; raw[3] = 0x00;
        memcpy(raw + 4, data, sizeof(data));
        TASK_CHECK(check_unit(raw, 4 + (int)sizeof(data)) == 0);
    }

    /* 4) pseudo-random units, biased towards 0x00 / 0xff runs */
    for (int iter = 0; iter < 64; iter++) {
        raw[0] = 0x00; raw[1] = 0x00; raw[2] = 0x01; raw[3] = 0x00;
        int n = 32 + (int)(rnd_u32() % 200);
        for (int i = 0; i < n; i++) {
            unsigned int r = rnd_u32();
            raw[4 + i] = (unsigned char)((r & 3) == 0 ? 0x00 : ((r & 3) == 1 ? 0xff : (r >> 8)));
        }
        TASK_CHECK(check_unit(raw, 4 + n) == 0);
    }

    /* 5) cross-chunk stability: after every single fed byte of the carry
     *    unit, the bytes already produced are a prefix of the batch output */
    len = make_lbac_unit(raw, 1, 0x7f, 9);
    {
        int na = batch_demulate(raw, len, a);
        enc_chunk_stream_t cs;
        enc_chunk_stream_begin(&cs, b, raw[3]);
        for (int i = 4; i < len; i++) {
            enc_chunk_stream_feed(&cs, raw[i]);
            TASK_CHECK(cs.out_len <= na);
            TASK_CHECK(memcmp(b, a, cs.out_len) == 0);
        }
        enc_chunk_stream_finish(&cs);
        TASK_CHECK(cs.out_len == na);
        TASK_CHECK(memcmp(b, a, na) == 0);
    }
    return 0;
}

/*-------------------------------------------------------------------------*/
/* lifecycle group                                                         */
/*-------------------------------------------------------------------------*/

static int test_lifecycle_single_frame(void)
{
    enc_cfg_t c;
    stream_cfg(&c, 128, 256);

    sink_t sink;
    sink_init(&sink);
    c.output_chunk        = cb_sink;
    c.output_chunk_opaque = &sink;

    int err = 0;
    void *h = uavs3e_create(&c, &err);
    TASK_CHECK(h != NULL);

    com_img_t *img = NULL;
    TASK_CHECK(uavs3e_get_img(h, &img) == COM_OK);
    fill_frame(img, 0);
    img->pts = 0;

    sink_frame_begin(&sink);
    enc_stat_t s;
    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h, &s, img) == COM_OK && s.bytes > 0);

    /* exactly one final chunk, as the last chunk of the frame */
    TASK_CHECK(sink.final_calls == 1);
    TASK_CHECK(sink.after_final == 0);
    TASK_CHECK(sink.calls >= 2);
    TASK_CHECK(sink.final_rows == 256 / 64);
    TASK_CHECK(sink.monotonic_ok);
    TASK_CHECK(sink.asm_len == s.bytes);
    TASK_CHECK(memcmp(sink.asm_buf, s.buf, s.bytes) == 0);

    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h, &s, NULL) == COM_OK_NO_MORE_FRM);
    TASK_CHECK(sink.final_calls == 1); /* no extra final on flush */
    uavs3e_free(h);
    return 0;
}

static int test_lifecycle_two_instances(void)
{
    enc_cfg_t c1, c2;
    stream_cfg(&c1, 128, 256);
    stream_cfg(&c2, 128, 128);
    c2.qp = 27;

    sink_t s1, s2;
    sink_init(&s1);
    sink_init(&s2);
    c1.output_chunk = cb_sink; c1.output_chunk_opaque = &s1;
    c2.output_chunk = cb_sink; c2.output_chunk_opaque = &s2;

    int err1 = 0, err2 = 0;
    void *h1 = uavs3e_create(&c1, &err1);
    void *h2 = uavs3e_create(&c2, &err2);
    TASK_CHECK(h1 != NULL && h2 != NULL);

    /* interleave the two encoders frame by frame */
    for (int f = 0; f < 2; f++) {
        com_img_t *img = NULL;
        enc_stat_t s;

        TASK_CHECK(uavs3e_get_img(h1, &img) == COM_OK);
        fill_frame(img, f);
        img->pts = f;
        sink_frame_begin(&s1);
        memset(&s, 0, sizeof(s));
        TASK_CHECK(uavs3e_enc(h1, &s, img) == COM_OK && s.bytes > 0);
        TASK_CHECK(s.poc == f);
        TASK_CHECK(s1.asm_len == s.bytes);
        TASK_CHECK(memcmp(s1.asm_buf, s.buf, s.bytes) == 0);

        TASK_CHECK(uavs3e_get_img(h2, &img) == COM_OK);
        fill_frame(img, f + 40);
        img->pts = f;
        sink_frame_begin(&s2);
        memset(&s, 0, sizeof(s));
        TASK_CHECK(uavs3e_enc(h2, &s, img) == COM_OK && s.bytes > 0);
        TASK_CHECK(s.poc == f);
        TASK_CHECK(s2.asm_len == s.bytes);
        TASK_CHECK(memcmp(s2.asm_buf, s.buf, s.bytes) == 0);
    }
    TASK_CHECK(s1.final_calls == 2 && s2.final_calls == 2);
    TASK_CHECK(s1.monotonic_ok && s2.monotonic_ok);
    TASK_CHECK(s1.after_final == 0 && s2.after_final == 0);

    enc_stat_t s;
    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h1, &s, NULL) == COM_OK_NO_MORE_FRM);
    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h2, &s, NULL) == COM_OK_NO_MORE_FRM);
    uavs3e_free(h1);
    uavs3e_free(h2);
    return 0;
}

static int test_lifecycle_null_callback(void)
{
    /* NULL callback: the original configuration works unchanged */
    enc_cfg_t c;
    stream_cfg(&c, 128, 256);
    TASK_CHECK(c.output_chunk == NULL);

    int err = 0;
    void *h = uavs3e_create(&c, &err);
    TASK_CHECK(h != NULL);

    com_img_t *img = NULL;
    TASK_CHECK(uavs3e_get_img(h, &img) == COM_OK);
    fill_frame(img, 0);
    img->pts = 0;
    enc_stat_t s;
    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h, &s, img) == COM_OK && s.bytes > 0 && s.buf != NULL);
    memset(&s, 0, sizeof(s));
    TASK_CHECK(uavs3e_enc(h, &s, NULL) == COM_OK_NO_MORE_FRM);
    uavs3e_free(h);

    /* a configuration outside the streaming envelope is rejected when the
     * callback is set, but stays usable without it */
    {
        enc_cfg_t bad;
        stream_cfg(&bad, 128, 256);
        bad.sao_enable = 1;
        err = 0;
        h = uavs3e_create(&bad, &err);
        TASK_CHECK(h != NULL); /* no callback: accepted as before */
        uavs3e_free(h);

        bad.output_chunk = cb_sink;
        err = 0;
        h = uavs3e_create(&bad, &err);
        TASK_CHECK(h == NULL);
        TASK_CHECK(err == COM_ERR_UNSUPPORTED);
    }

    /* every constraint of the streaming envelope is enforced */
    {
        struct { int field; int value; } violations[] = {
            { 0, 1 }, /* sao_enable        */
            { 1, 1 }, /* alf_enable        */
            { 2, 1 }, /* use_pic_sign      */
            { 3, 1 }, /* adaptive_dqp      */
            { 4, 1 }, /* max_b_frames      */
            { 5, 2 }, /* wpp_threads       */
            { 6, 2 }, /* frm_threads       */
            { 7, 1 }, /* rc_type           */
            { 8, 10 },/* bit_depth_input   */
        };
        for (unsigned i = 0; i < sizeof(violations) / sizeof(violations[0]); i++) {
            enc_cfg_t v;
            stream_cfg(&v, 128, 256);
            switch (violations[i].field) {
            case 0: v.sao_enable        = violations[i].value; break;
            case 1: v.alf_enable        = violations[i].value; break;
            case 2: v.use_pic_sign      = violations[i].value; break;
            case 3: v.adaptive_dqp      = violations[i].value; break;
            case 4: v.max_b_frames      = violations[i].value; break;
            case 5: v.wpp_threads       = violations[i].value; break;
            case 6: v.frm_threads       = violations[i].value; break;
            case 7: v.rc_type           = violations[i].value; break;
            case 8: v.bit_depth_input   = violations[i].value; break;
            }
            v.output_chunk = cb_sink;
            err = 0;
            h = uavs3e_create(&v, &err);
            TASK_CHECK(h == NULL);
            TASK_CHECK(err == COM_ERR_UNSUPPORTED);
        }
    }
    return 0;
}

/*-------------------------------------------------------------------------*/

const struct task_case task_cases[] = {
    { "prefix",    "chunk_timing_and_prefix",   test_prefix_timing            },
    { "coding",    "stream_matches_wholeframe", test_coding_sequence          },
    { "coding",    "demulate_state_boundaries", test_coding_demulate          },
    { "lifecycle", "single_frame_final",        test_lifecycle_single_frame   },
    { "lifecycle", "two_instances_interleaved", test_lifecycle_two_instances  },
    { "lifecycle", "null_callback_and_reject",  test_lifecycle_null_callback  },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
